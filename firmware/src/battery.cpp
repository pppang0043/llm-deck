#include "battery.h"

#include <Preferences.h>

#include "pins.h"

// 리튬 배터리 전압(mV) → 남은 양(%). 전압이 % 에 정비례하지 않아서 표로 잇는다.
static const int CURVE[][2] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3920, 70}, {3860, 60}, {3820, 50},
    {3790, 40},  {3760, 30}, {3730, 20}, {3690, 10}, {3600, 5},  {3300, 0},
};
#define CURVE_N (sizeof(CURVE) / sizeof(CURVE[0]))

// 이보다 낮으면 배터리가 꽂혀 있지 않은 것으로 본다
#define NO_BATTERY_MV 2500

static float smooth_mv = 0;
static int pct = -1;

// 측정 보정 배율. 전압을 1/3 로 나누는 저항(오차 1%)과 ESP32 ADC 의 오차가 곱해져 수십~100mV 가 틀어질 수 있어서,
// 실제 전압을 아는 순간의 값으로 배율을 구해 저장해 둔다.
//  - 자동: USB 가 꽂혀 있는데 전압이 한참 그대로면 충전이 끝난 것(또는 막바지)이다. 충전 칩(ETA6098)은
//    4.1V 아래로 내려가면 다시 충전을 시작하므로, 이때 실제 전압은 4.1~4.2V 사이 → 4.15V 로 본다.
//  - 수동: PC 도우미로 멀티미터 값을 알려 주면 그 값으로 맞춘다 (자동보다 우선).
#define FULL_REST_MV 4150
#define FLAT_MINUTES 20  // 이 시간 동안
#define FLAT_RANGE_MV 12 // 이 폭 안에서만 움직이면 "그대로"
static Preferences prefs;
static float scale = 1.0f;
static bool scale_manual = false;
static int minute_mv[FLAT_MINUTES];
static int minute_n = 0;
static bool full = false;

// 충전 중인지. 보드의 충전 칩 상태선은 LED 에만 연결되어 있어 직접 읽을 수 없으므로 두 가지로 짐작한다.
//  1) USB 로 PC 에 연결되어 있음 (USB 신호가 오가는지로 안다) → 그 USB 로 전원도 들어온다
//  2) 충전기(어댑터)만 꽂았을 때: 꽂는 순간 전압이 갑자기 오르고, 뽑는 순간 갑자기 떨어진다
#define JUMP_MV 80
#define WINDOW 10  // 최근 30초 (3초 x 10)
static int recent[WINDOW];
static int recent_n = 0;
static bool charger_by_voltage = false;
static bool charging = false;

// 보정 전 전압 (mV). 64번 재서 크고 작은 쪽 1/4 씩을 버리고 가운데만 평균한다 (튀는 값 제거).
static int read_raw_mv() {
  uint16_t v[64];
  for (int i = 0; i < 64; i++) v[i] = analogReadMilliVolts(BAT_ADC);
  // 삽입 정렬 (64개라 충분히 빠르다)
  for (int i = 1; i < 64; i++) {
    uint16_t x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = x;
  }
  uint32_t sum = 0;
  for (int i = 16; i < 48; i++) sum += v[i];
  return (int)(sum * BAT_DIVIDER / 32);
}

static int raw_mv = 0;  // 마지막으로 잰 보정 전 값

static int read_mv() {
  raw_mv = read_raw_mv();
  return (int)(raw_mv * scale + 0.5f);
}

static void set_scale(float value, bool manual) {
  // 부품 오차로 생길 수 있는 범위를 넘으면 잘못 잰 것으로 보고 무시한다
  if (value < 0.9f || value > 1.1f) {
    Serial.printf("[battery] calibration %.4f out of range, ignored\n", value);
    return;
  }
  scale = value;
  scale_manual = manual;
  prefs.putFloat("bat_scale", scale);
  prefs.putBool("bat_manual", manual);
  smooth_mv = 0;
  Serial.printf("[battery] calibrated (%s): scale %.4f\n", manual ? "manual" : "auto", scale);
}

void battery_calibrate(int actual_mv) {
  if (actual_mv <= 0) {
    // 0 이면 보정을 지우고 처음 상태로 (자동 보정은 다시 동작한다)
    scale = 1.0f;
    scale_manual = false;
    prefs.remove("bat_scale");
    prefs.remove("bat_manual");
    smooth_mv = 0;
    Serial.println("[battery] calibration cleared");
    return;
  }
  int raw = read_raw_mv();
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
  if (full || minute_n < FLAT_MINUTES) return;
  int lo = minute_mv[0], hi = minute_mv[0];
  long sum = 0;
  for (int i = 0; i < minute_n; i++) {
    lo = min(lo, minute_mv[i]);
    hi = max(hi, minute_mv[i]);
    sum += minute_mv[i];
  }
  if (hi - lo > FLAT_RANGE_MV) return;
  full = true;
  Serial.printf("[battery] charge looks complete (raw %d..%d mV)\n", lo, hi);
  if (!scale_manual) {
    float value = (float)FULL_REST_MV / (sum / minute_n);
    // 저장 횟수를 줄이려고 0.3% 넘게 달라질 때만 바꾼다
    if (fabsf(value - scale) / scale > 0.003f) set_scale(value, false);
  }
}

static int mv_to_pct(int mv) {
  if (mv >= CURVE[0][0]) return 100;
  for (size_t i = 1; i < CURVE_N; i++) {
    if (mv >= CURVE[i][0]) {
      int v0 = CURVE[i][0], v1 = CURVE[i - 1][0];
      int p0 = CURVE[i][1], p1 = CURVE[i - 1][1];
      return p0 + (mv - v0) * (p1 - p0) / (v1 - v0);
    }
  }
  return 0;
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
    } else if (charger_by_voltage && hi - mv >= JUMP_MV && hi - prev >= JUMP_MV) {
      charger_by_voltage = false;
      recent_n = 0;
    }
  }
  if (recent_n == WINDOW) {
    memmove(recent, recent + 1, sizeof(int) * (WINDOW - 1));
    recent_n--;
  }
  recent[recent_n++] = mv;
}

static void update(int mv) {
  if (mv < NO_BATTERY_MV) {
    smooth_mv = 0;
    pct = -1;
    charging = false;
    recent_n = 0;
    return;
  }
  bool was_charging = charging;
  detect_charger(mv);
  charging = Serial.isPlugged() || charger_by_voltage;
  // 충전기를 꽂거나 뽑으면 전압이 한 번에 바뀌므로 다듬은 값도 새로 시작한다
  if (charging != was_charging) {
    smooth_mv = 0;
    full = false;
    minute_n = 0;
    Serial.printf("[battery] %s (%d mV)\n", charging ? "charging" : "on battery", mv);
  }
  // 무선 송신 등으로 전압이 출렁이므로 천천히 따라간다
  smooth_mv = smooth_mv == 0 ? mv : smooth_mv * 0.8f + mv * 0.2f;
  // 충전이 끝났으면 꽉 찬 것이다 (전압만으로 계산하면 쉬는 동안 조금 낮게 나온다)
  pct = full ? 100 : mv_to_pct((int)smooth_mv);
}

void battery_init() {
  prefs.begin("llm-deck");
  scale = prefs.getFloat("bat_scale", 1.0f);
  scale_manual = prefs.getBool("bat_manual", false);
  // 감쇠는 기본값(11dB, 약 3.1V 까지)을 그대로 쓴다
  update(read_mv());
  Serial.printf("[battery] %d mV, %d%%, %s, scale %.4f%s\n", battery_mv(), pct, charging ? "charging" : "on battery",
                scale, scale_manual ? " (manual)" : "");
}

void battery_loop() {
  static uint32_t checked_ms = 0;
  if (millis() - checked_ms < 3000) return;
  checked_ms = millis();
  int mv = read_mv();
  update(mv);
  // 1분마다 기록을 남긴다 (충전/방전 추세 확인용)
  static uint32_t logged_ms = 0;
  if (millis() - logged_ms >= 60000) {
    logged_ms = millis();
    check_full(raw_mv);
    Serial.printf("[battery] %d mV (now %d, raw %d), %d%%, %s%s\n", battery_mv(), mv, raw_mv, pct,
                  charging ? "charging" : "on battery", full ? ", full" : "");
  }
}

int battery_mv() {
  return (int)smooth_mv;
}

int battery_pct() {
  return pct;
}

bool battery_charging() {
  return charging;
}
