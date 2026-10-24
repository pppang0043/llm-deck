#include "battery.h"

#include <Preferences.h>

#include "battery_level.h"
#include "pins.h"
#include "usb_link.h"

// 이보다 낮으면 배터리가 꽂혀 있지 않은 것으로 본다
#define NO_BATTERY_MV 2500

static float smooth_mv = 0;
static BatteryLevel level;

// 측정 보정 배율. 전압을 1/3 로 나누는 저항(오차 1%)과 ESP32 ADC 의 오차가 곱해져 수십~100mV 가 틀어질 수 있어서,
// 실제 전압을 아는 순간의 값으로 배율을 구해 저장해 둔다.
//  - 자동: USB 가 꽂혀 있는데 전압이 한참 그대로면 충전이 끝난 것(또는 막바지)이다. 충전 칩(ETA6098)은
//    4.1V 아래로 내려가면 다시 충전을 시작하므로, 이때 실제 전압은 4.1~4.2V 사이 → FULL_REST_MV(4.15V) 로 본다.
//    한 번 잰 값은 10mV 쯤 흔들리므로 전에 쓰던 값과 반씩 섞는다.
//  - 수동: PC 도우미로 멀티미터 값을 알려 주면 그 값으로 맞춘다 (자동보다 우선).
#define FLAT_MINUTES 20  // 이 시간 동안
#define FLAT_RANGE_MV 12 // 이 폭 안에서만 움직이면 "그대로"
static Preferences prefs;
static float scale = 1.0f;
static bool scale_saved = false;
static bool scale_manual = false;
static int minute_mv[FLAT_MINUTES];
static int minute_n = 0;
static bool full = false;

// 전원이 들어오는지. 보드의 충전 칩 상태선은 LED 에만 연결되어 있어 직접 읽을 수 없으므로 세 가지로 짐작한다.
//  1) USB 신호가 오간다 → 그 USB 로 전원도 들어온다
//  2) PC 가 USB 로 무언가 보냈다 (최근 PC_RX_MS 안). PC 가 잠들거나 하면 USB 신호가 몇 분씩 끊긴 것처럼 보이는데,
//     그동안에도 전원은 그대로 들어와서 "배터리 사용 중" 으로 잘못 바뀌곤 했다.
//     단, 그 뒤에 전압이 갑자기 떨어졌으면 케이블을 뽑은 것이다.
//  3) 충전기(어댑터)만 꽂았을 때: 꽂는 순간 전압이 갑자기 오르고, 뽑는 순간 갑자기 떨어진다
#define PC_RX_MS 45000  // 도우미는 30초마다 보낸다
#define JUMP_MV 80
#define WINDOW 10  // 최근 30초 (3초 x 10)
static int recent[WINDOW];
static int recent_n = 0;
static bool charger_by_voltage = false;
static uint32_t dropped_ms = 0;  // USB 신호가 없을 때 전압이 갑자기 떨어진 때
static bool charging = false;

// 한 번 잴 때 보정 전 전압 (mV). 무선 송신 때 잠깐 출렁이는 값에 덜 걸리도록 약 5ms 동안 나눠 32번 재고,
// 크고 작은 쪽 1/4 씩을 버리고 가운데만 평균한다.
static int read_once_mv() {
  uint16_t v[32];
  for (int i = 0; i < 32; i++) {
    v[i] = analogReadMilliVolts(BAT_ADC);
    delayMicroseconds(150);
  }
  // 삽입 정렬 (32개라 충분히 빠르다)
  for (int i = 1; i < 32; i++) {
    uint16_t x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = x;
  }
  uint32_t sum = 0;
  for (int i = 8; i < 24; i++) sum += v[i];
  return (int)(sum * BAT_DIVIDER / 16);
}

// 최근 다섯 번(15초) 잰 값의 가운데 값. 카메라를 켜는 순간처럼 한두 번 크게 튀는 값을 버린다.
#define MEDIAN_N 5
static int last_raw[MEDIAN_N];
static int last_raw_n = 0;

static int read_raw_mv() {
  int mv = read_once_mv();
  if (last_raw_n == MEDIAN_N) {
    memmove(last_raw, last_raw + 1, sizeof(int) * (MEDIAN_N - 1));
    last_raw_n--;
  }
  last_raw[last_raw_n++] = mv;
  int v[MEDIAN_N];
  memcpy(v, last_raw, sizeof(int) * last_raw_n);
  for (int i = 1; i < last_raw_n; i++) {
    int x = v[i], j = i - 1;
    while (j >= 0 && v[j] > x) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = x;
  }
  return v[last_raw_n / 2];
}

static int raw_mv = 0;  // 마지막으로 잰 보정 전 값

static int read_mv() {
  raw_mv = read_raw_mv();
  return (int)(raw_mv * scale + 0.5f);
}

// 전원을 꽂거나 뽑으면 전압이 한 번에 바뀌므로 지난 값들을 버린다
static void restart_readings() {
  last_raw_n = 0;
  smooth_mv = 0;
}

static void set_scale(float value, bool manual) {
  // 부품 오차로 생길 수 있는 범위를 넘으면 잘못 잰 것으로 보고 무시한다
  if (value < 0.9f || value > 1.1f) {
    Serial.printf("[battery] calibration %.4f out of range, ignored\n", value);
    return;
  }
  float old = scale;
  scale = value;
  scale_manual = manual;
  scale_saved = true;
  prefs.putFloat("bat_scale", scale);
  prefs.putBool("bat_manual", manual);
  // 다듬던 값은 새 배율로 옮긴다 (지우면 다음 3초 동안 0 mV 로 보인다)
  last_raw_n = 0;
  smooth_mv = smooth_mv * scale / old;
  Serial.printf("[battery] calibrated (%s): scale %.4f\n", manual ? "manual" : "auto", scale);
}

void battery_calibrate(int actual_mv) {
  if (actual_mv <= 0) {
    // 0 이면 보정을 지우고 처음 상태로 (자동 보정은 다시 동작한다)
    scale = 1.0f;
    scale_manual = false;
    scale_saved = false;
    prefs.remove("bat_scale");
    prefs.remove("bat_manual");
    restart_readings();
    Serial.println("[battery] calibration cleared");
    return;
  }
  int raw = read_once_mv();
  if (raw < NO_BATTERY_MV) return;
  set_scale((float)actual_mv / raw, true);
}

// 1분마다 불린다. USB 가 꽂힌 채 FLAT_MINUTES 동안 전압이 그대로면 충전이 끝난 것으로 본다.
static void check_full(int raw) {
  if (!charging) {
    minute_n = 0;
    full = false;
    return;
  }
  if (minute_n == FLAT_MINUTES) {
    memmove(minute_mv, minute_mv + 1, sizeof(int) * (FLAT_MINUTES - 1));
    minute_n--;
  }
  minute_mv[minute_n++] = raw;
  int lo = minute_mv[0], hi = minute_mv[0];
  long sum = 0;
  for (int i = 0; i < minute_n; i++) {
    lo = min(lo, minute_mv[i]);
    hi = max(hi, minute_mv[i]);
    sum += minute_mv[i];
  }
  if (full) {
    // 꽂아 둔 채 시간이 지나 배터리가 조금 줄면 충전 칩이 다시 충전을 시작한다 → 전압이 오른다
    if (raw - lo > FLAT_RANGE_MV * 2) {
      full = false;
      minute_n = 0;
      Serial.println("[battery] charging again");
    }
    return;
  }
  if (minute_n < FLAT_MINUTES || hi - lo > FLAT_RANGE_MV) return;
  full = true;
  Serial.printf("[battery] charge looks complete (raw %d..%d mV)\n", lo, hi);
  if (!scale_manual) {
    float value = (float)FULL_REST_MV / (sum / minute_n);
    // 20분 평탄은 충전이 끝났을 때 말고도 (PC USB 전원이 약해 전류가 막힐 때 등) 생겨서, 그때마다 배율이
    // 1.003~1.031 로 흔들렸다. 처음이 아니면 한 번에 0.5% 까지만 옮겨 여러 번의 값에 천천히 맞춘다
    if (scale_saved) value = constrain(value, scale * 0.995f, scale * 1.005f);
    // 저장 횟수를 줄이려고 0.3% 넘게 달라질 때만 바꾼다
    if (fabsf(value - scale) / scale > 0.003f) set_scale(value, false);
  }
}

static void detect_charger(int mv) {
  if (recent_n >= 2) {
    int lo = recent[0], hi = recent[0];
    for (int i = 1; i < recent_n; i++) {
      lo = min(lo, recent[i]);
      hi = max(hi, recent[i]);
    }
    int prev = recent[recent_n - 1];
    // 한 번 튄 값에 속지 않도록 직전 값과 이번 값이 둘 다 넘어야 한다
    if (!charger_by_voltage && mv - lo >= JUMP_MV && prev - lo >= JUMP_MV) {
      charger_by_voltage = true;
      recent_n = 0;
    } else if (hi - mv >= JUMP_MV && hi - prev >= JUMP_MV) {
      charger_by_voltage = false;
      if (!usb_host_present()) dropped_ms = millis() ? millis() : 1;
      recent_n = 0;
    }
  }
  if (recent_n == WINDOW) {
    memmove(recent, recent + 1, sizeof(int) * (WINDOW - 1));
    recent_n--;
  }
  recent[recent_n++] = mv;
}

static bool external_power() {
  if (charger_by_voltage || usb_host_present()) {
    dropped_ms = 0;
    return true;
  }
  uint32_t rx = usb_last_rx_ms();
  if (!rx || millis() - rx > PC_RX_MS) return false;
  // 뽑아서 전압이 떨어진 뒤로 PC 가 보낸 것이 없으면 배터리로 쓰는 중이다
  return !dropped_ms || (int32_t)(rx - dropped_ms) > 0;
}

static void update(int mv) {
  if (mv < NO_BATTERY_MV) {
    restart_readings();
    level.update(0, false, false, millis());
    charging = false;
    recent_n = 0;
    return;
  }
  bool was_charging = charging;
  detect_charger(mv);
  charging = external_power();
  if (charging != was_charging) {
    restart_readings();
    full = false;
    minute_n = 0;
    Serial.printf("[battery] %s (%d mV)\n", charging ? "charging" : "on battery", mv);
  }
  // 무선 송신 등으로 전압이 출렁이므로 천천히 따라간다
  smooth_mv = smooth_mv == 0 ? mv : smooth_mv * 0.8f + mv * 0.2f;
  level.update((int)smooth_mv, charging, full, millis());
}

void battery_init() {
  prefs.begin("llm-deck");
  scale_saved = prefs.isKey("bat_scale");
  scale = prefs.getFloat("bat_scale", 1.0f);
  scale_manual = prefs.getBool("bat_manual", false);
  // 끄기 전 충전 중이었으면 그때 잔량부터 (save_level). 배터리로 쓰다 꺼졌으면 저장값이 -1 이다
  level.resume(prefs.getChar("bat_resume", -1));
  // 감쇠는 기본값(11dB, 약 3.1V 까지)을 그대로 쓴다
  update(read_mv());
  Serial.printf("[battery] %d mV, %d%%, %s, scale %.4f%s\n", battery_mv(), battery_pct(),
                charging ? "charging" : "on battery", scale, scale_manual ? " (manual)" : "");
}

// ---- 남은 시간 짐작 ----
// 배터리로만 쓰는 동안 1분마다 잔량을 적어 두고, 최근 40분(모자라면 뽑은 뒤부터) 동안 줄어든 빠르기로 나눈다.
// 잔량이 전압으로 짐작한 값이라 처음 몇 %는 흔들리므로, 3% 이상 줄어든 뒤에야 알려 준다.
#define DRAIN_SAMPLES 41
#define DRAIN_MIN_DROP 3
static uint8_t drain_pct[DRAIN_SAMPLES];
static int drain_count = 0, drain_head = 0;
static int start_pct = -1;
static uint32_t start_min = 0;
static int minutes_left = -1;

static void track_drain() {
  uint32_t now_min = millis() / 60000;
  int pct = battery_pct();
  if (charging || pct < 0) {
    drain_count = 0;
    start_pct = -1;
    minutes_left = -1;
    return;
  }
  if (start_pct < 0) {
    start_pct = pct;
    start_min = now_min;
  }
  // 배터리로 쓰는 동안 잔량은 무거운 일이 끝나 다시 오를 때가 있는데, 남은 시간은 줄어든 빠르기로만 본다
  int now_pct = pct;
  if (drain_count > 0) {
    int last = drain_pct[(drain_head - 1 + DRAIN_SAMPLES) % DRAIN_SAMPLES];
    if (now_pct > last) now_pct = last;
  }
  drain_pct[drain_head] = now_pct;
  drain_head = (drain_head + 1) % DRAIN_SAMPLES;
  if (drain_count < DRAIN_SAMPLES) drain_count++;
  // 창 안의 가장 오래된 값과 지금 값
  int oldest = drain_pct[(drain_head - drain_count + DRAIN_SAMPLES) % DRAIN_SAMPLES];
  int span = drain_count - 1;
  int drop = oldest - now_pct;
  if (drop < DRAIN_MIN_DROP) {
    // 창이 짧으면(적게 쓰는 중) 뽑은 뒤 전체로 본다
    drop = start_pct - now_pct;
    span = now_min - start_min;
  }
  if (drop < DRAIN_MIN_DROP || span <= 0) {
    minutes_left = -1;
    return;
  }
  minutes_left = (int)((long)now_pct * span / drop);
}

int battery_minutes_left() {
  return minutes_left;
}

// 충전 중 잔량이 바뀔 때마다 적어 둔다 (다시 켜도 이어 가게). 배터리로 쓰는 중이면 지운다.
// 충전 한 번에 100번 남짓 쓰므로 NVS 수명에는 문제없다
static void save_level() {
  static int saved = -2;
  // 켠 직후에는 USB 를 아직 못 알아봐 "배터리 사용 중" 일 수 있다. 그때 지우지 않게 30초 기다린다.
  // 충전이 끝나도(full) 꽂혀 있는 동안은 적어 둔다
  if (millis() < 30000) return;
  int value = charging ? battery_pct() : -1;
  if (value == saved) return;
  saved = value;
  prefs.putChar("bat_resume", value);
}

void battery_loop() {
  static uint32_t checked_ms = 0;
  if (millis() - checked_ms < 3000) return;
  checked_ms = millis();
  int mv = read_mv();
  update(mv);
  save_level();
  // 1분마다 기록을 남긴다 (충전/방전 추세 확인용)
  static uint32_t logged_ms = 0;
  if (millis() - logged_ms >= 60000) {
    logged_ms = millis();
    check_full(raw_mv);
    track_drain();
    Serial.printf("[battery] %d mV (now %d, raw %d), %d%% (voltage %d%%), %s%s\n", battery_mv(), mv, raw_mv,
                  battery_pct(), battery_mv_to_pct(battery_mv()), charging ? "charging" : "on battery",
                  full ? ", full" : "");
    if (charging) Serial.printf("[battery] charge offset %d mV\n", level.charge_offset_mv());
  }
}

int battery_mv() {
  return (int)smooth_mv;
}

int battery_pct() {
  return level.pct();
}

bool battery_charging() {
  // 충전기는 꽂혀 있어도 충전이 끝났으면 충전 중이 아니다
  return charging && !full;
}

bool battery_external_power() {
  return charging;
}
