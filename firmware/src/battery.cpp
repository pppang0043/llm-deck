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

static int read_mv() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BAT_ADC);
  return sum / 16 * BAT_DIVIDER;
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

static void update(int mv) {
  if (mv < NO_BATTERY_MV) {
    smooth_mv = 0;
    pct = -1;
    return;
  }
  // 무선 송신 등으로 전압이 출렁이므로 천천히 따라간다
  smooth_mv = smooth_mv == 0 ? mv : smooth_mv * 0.8f + mv * 0.2f;
  pct = mv_to_pct((int)smooth_mv);
}

void battery_init() {
  // 감쇠는 기본값(11dB, 약 3.1V 까지)을 그대로 쓴다
  update(read_mv());
  Serial.printf("[battery] %d mV, %d%%\n", battery_mv(), pct);
}

void battery_loop() {
  static uint32_t checked_ms = 0;
  if (millis() - checked_ms < 3000) return;
  checked_ms = millis();
  update(read_mv());
}

int battery_mv() {
  return (int)smooth_mv;
}

int battery_pct() {
  return pct;
}
