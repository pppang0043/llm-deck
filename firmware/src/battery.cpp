#include "battery.h"

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

// 충전 중인지. 보드의 충전 칩 상태선은 LED 에만 연결되어 있어 직접 읽을 수 없으므로 두 가지로 짐작한다.
//  1) USB 로 PC 에 연결되어 있음 (USB 신호가 오가는지로 안다) → 그 USB 로 전원도 들어온다
//  2) 충전기(어댑터)만 꽂았을 때: 꽂는 순간 전압이 갑자기 오르고, 뽑는 순간 갑자기 떨어진다
#define JUMP_MV 80
#define WINDOW 10  // 최근 30초 (3초 x 10)
static int recent[WINDOW];
static int recent_n = 0;
static bool charger_by_voltage = false;
static bool charging = false;

static int read_mv() {
  uint32_t sum = 0;
  for (int i = 0; i < 64; i++) sum += analogReadMilliVolts(BAT_ADC);
  return sum / 64 * BAT_DIVIDER;
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
    Serial.printf("[battery] %s (%d mV)\n", charging ? "charging" : "on battery", mv);
  }
  // 무선 송신 등으로 전압이 출렁이므로 천천히 따라간다
  smooth_mv = smooth_mv == 0 ? mv : smooth_mv * 0.8f + mv * 0.2f;
  pct = mv_to_pct((int)smooth_mv);
}

void battery_init() {
  // 감쇠는 기본값(11dB, 약 3.1V 까지)을 그대로 쓴다
  update(read_mv());
  Serial.printf("[battery] %d mV, %d%%, %s\n", battery_mv(), pct, charging ? "charging" : "on battery");
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
    Serial.printf("[battery] %d mV (now %d), %d%%, %s\n", battery_mv(), mv, pct, charging ? "charging" : "on battery");
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
