#include "battery_level.h"

// 리튬 배터리 전압(mV) → 남은 양(%). 전압이 % 에 정비례하지 않아서 표로 잇는다.
// 맨 위는 충전 칩이 충전을 끝낸 뒤의 전압 (자동 보정이 FULL_REST_MV 에 맞춘다).
static const int CURVE[][2] = {
    {FULL_REST_MV, 100}, {4100, 90}, {4000, 80}, {3920, 70}, {3860, 60}, {3820, 50},
    {3790, 40},          {3760, 30}, {3730, 20}, {3690, 10}, {3600, 5},  {3300, 0},
};
#define CURVE_N (sizeof(CURVE) / sizeof(CURVE[0]))

// 꽂은 뒤 이만큼 지나 전압이 얼마나 올랐는지 잰다. 충전 전류를 받은 배터리 전압은 몇 분에 걸쳐 오르므로
// (45초로 쟀을 때 실제보다 덜 빼서 1분에 3~7% 씩 오르는 것으로 보였다) 2분 기다린다.
#define OFFSET_AFTER_MS 120000
// 충전 중 잔량이 오를 수 있는 가장 빠른 빠르기 (1분에 1%, 0%에서 다 차는 데 100분). 이보다 빨리 차는 배터리면
// 화면이 늦게 따라가다 충전이 끝날 때 100% 로 바뀐다
#define CHARGE_STEP_MS 60000
#define OFFSET_MAX_MV 300
// 배터리로 쓸 때 내려가는 빠르기. 크게 차이 나면(거의 빈 배터리는 전압이 빨리 떨어진다) 바로 따라간다
#define STEP_DOWN_MS 15000
#define FAST_DOWN_PCT 5
// 이만큼 높게 짐작되는 일이 이만큼 이어지면 올린다
#define RISE_PCT 3
#define RISE_AFTER_MS 300000
// 충전 끝 전압에 닿은 뒤 1% 씩 올리는 간격
#define CREEP_MS 120000
#define CREEP_NEAR_MV 30

int battery_mv_to_pct(int mv) {
  if (mv >= CURVE[0][0]) return 100;
  for (unsigned i = 1; i < CURVE_N; i++) {
    if (mv >= CURVE[i][0]) {
      int v0 = CURVE[i][0], v1 = CURVE[i - 1][0];
      int p0 = CURVE[i][1], p1 = CURVE[i - 1][1];
      return p0 + (mv - v0) * (p1 - p0) / (v1 - v0);
    }
  }
  return 0;
}

static int charge_pct(int mv, int offset) {
  int pct = battery_mv_to_pct(mv - offset);
  return pct > 99 ? 99 : pct;
}

int BatteryLevel::update(int mv, bool charging, bool full, uint32_t now) {
  if (mv <= 0) {
    shown_ = -1;
    started_ = false;
    measuring_ = false;
    return shown_;
  }
  if (!started_) {
    shown_ = charging ? charge_pct(mv, offset_mv_) : battery_mv_to_pct(mv);
    if (charging && resume_pct_ > shown_) shown_ = resume_pct_ > 99 ? 99 : resume_pct_;
    resume_pct_ = -1;
  } else if (charging != was_charging_) {
    // 꽂은 순간: 바로 앞 배터리 전압과 잠시 뒤 전압의 차이가 충전 전류로 오른 만큼이다
    measuring_ = charging && battery_mv_ > 0 && !skip_measure_;
    skip_measure_ = false;
    before_mv_ = battery_mv_;
    plug_ms_ = now;
  }
  if (!started_ || charging != was_charging_) {
    started_ = true;
    was_charging_ = charging;
    step_ms_ = creep_ms_ = now;
    above_ms_ = 0;
  }

  if (full) {
    shown_ = 100;
    return shown_;
  }

  if (charging) {
    if (measuring_) {
      // 재는 동안은 그대로 둔다 (짐작한 값으로 올려 두면 되돌릴 수 없다)
      if (now - plug_ms_ < OFFSET_AFTER_MS) return shown_;
      measuring_ = false;
      int rise = mv - before_mv_;
      if (rise >= 0 && rise <= OFFSET_MAX_MV) offset_mv_ = rise;
    }
    int est = charge_pct(mv, offset_mv_);
    if (est > shown_ && now - step_ms_ >= CHARGE_STEP_MS) {
      step_ms_ = now;
      shown_++;
    }
    if (mv >= FULL_REST_MV - CREEP_NEAR_MV) {
      if (now - creep_ms_ >= CREEP_MS) {
        creep_ms_ = now;
        if (shown_ < 99) shown_++;
        step_ms_ = now;
      }
    } else {
      creep_ms_ = now;
    }
    return shown_;
  }

  battery_mv_ = mv;
  int est = battery_mv_to_pct(mv);
  if (est < shown_) {
    above_ms_ = 0;
    if (shown_ - est >= FAST_DOWN_PCT || now - step_ms_ >= STEP_DOWN_MS) {
      step_ms_ = now;
      shown_--;
    }
  } else if (est >= shown_ + RISE_PCT) {
    if (!above_ms_) {
      above_ms_ = now ? now : 1;
    } else if (now - above_ms_ >= RISE_AFTER_MS) {
      shown_ = est;
      above_ms_ = 0;
    }
  } else {
    above_ms_ = 0;
    step_ms_ = now;
  }
  return shown_;
}
