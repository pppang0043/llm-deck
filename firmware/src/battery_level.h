#pragma once

#include <stdint.h>

// 충전이 끝난 배터리의 전압 (mV, 보정한 값). 자동 보정이 이 값에 맞춘다 (battery.cpp).
#define FULL_REST_MV 4150

// 리튬 배터리 전압(mV) → 남은 양(%)
int battery_mv_to_pct(int mv);

// 화면에 보일 잔량을 정한다. 하드웨어를 쓰지 않아 PC 에서도 시험할 수 있다.
// 전압으로 짐작한 값은 부하(화면 밝기, 무선, 카메라)와 충전 전류 때문에 수십 mV 씩 흔들려서 그대로 쓰면 오르내린다.
//  - 배터리로 쓸 때: 줄기만 한다 (15초에 1% 씩 따라 내려감). 무거운 일이 끝나 전압이 돌아와
//    3% 넘게 높게 짐작되는 일이 5분 이어질 때만 올린다.
//  - 충전 중: 늘기만 하고, 1분에 1% 보다 빨리 오르지 않는다. 충전 전류로 오른 전압(꽂고 2분 동안 오른 만큼)을 빼고 계산하고,
//    전압이 충전 끝 전압에 닿은 뒤(정전압 충전)에는 2분에 1% 씩 올린다. 충전이 끝나기 전에는 99% 까지만.
class BatteryLevel {
 public:
  // mv: 다듬은 전압 (0 이면 배터리 없음). full: 충전이 끝났음. 돌려주는 값은 0~100, 배터리가 없으면 -1.
  int update(int mv, bool charging, bool full, uint32_t now_ms);
  int pct() const { return shown_; }
  // 충전 전류 때문에 오른 전압으로 쓰는 값 (mV)
  int charge_offset_mv() const { return offset_mv_; }
  // 다시 켜기 전에 충전 중이었고 그때 보이던 잔량. 충전 중에 다시 켜졌으면 첫 값을 이것보다 낮추지 않는다
  // (충전 중 전압으로는 정전압 충전 구간을 가릴 수 없어, 켤 때마다 100% 가 80% 대로 떨어졌다)
  void resume(int pct) { resume_pct_ = pct; }

 private:
  int shown_ = -1;
  bool started_ = false;
  bool was_charging_ = false;
  int offset_mv_ = 100;  // 처음 꽂는 것을 보기 전까지 쓰는 값
  int resume_pct_ = -1;
  int battery_mv_ = 0;   // 배터리로 쓸 때 마지막 전압 (꽂는 순간 얼마나 오르는지 재는 데 쓴다)
  int before_mv_ = 0;
  bool measuring_ = false;
  uint32_t plug_ms_ = 0;
  uint32_t step_ms_ = 0;
  uint32_t above_ms_ = 0;
  uint32_t creep_ms_ = 0;
};
