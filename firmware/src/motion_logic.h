#pragma once
#include <math.h>
#include <stdint.h>

// 기울기 센서 값으로 화면 방향과 들어 올림을 판단한다 (하드웨어 없이 시험할 수 있게 따로 둔다, tests/motion_logic_test.cpp).

// 화면 회전: 자이로로 크게 돌린 뒤 멈추면 바로(약 0.2초), 아니면 같은 방향이 1초 유지될 때 바꾼다.
// 자이로가 계속 움직이면(걷기·흔들기) 어느 쪽으로도 바꾸지 않는다. 자이로가 없으면(rate_dps < 0) 예전처럼 1초만 본다.
class TurnSettle {
 public:
  // 이보다 빠르면 돌리는 중 (도/초)
  static constexpr float TURN_DPS = 90;
  // 이보다 느리면 멈춘 것 (손 떨림은 몇 도/초)
  static constexpr float QUIET_DPS = 25;
  // 이만큼 이상 돌아야 일부러 돌린 것으로 본다 (도)
  static constexpr float TURN_DEG = 45;
  // 돌린 뒤 이만큼 멈춰 있고 방향도 그대로면 바꾼다
  static constexpr uint32_t SETTLE_MS = 160;
  // 돌린 것을 기억하는 시간 (멈춘 뒤 이 안에 방향이 정해지지 않으면 느린 판단으로)
  static constexpr uint32_t TURN_KEEP_MS = 1000;
  // 느린 판단: 방향과 멈춤이 이만큼 유지되어야 바꾼다
  static constexpr uint32_t SLOW_MS = 1000;

  // seen: 기울기로 본 방향(애매하면 -1), current: 지금 화면 방향. 바꿀 방향을 돌려주거나 -1.
  int sample(uint32_t now_ms, float rate_dps, int seen, int current) {
    uint32_t dt = started_ ? now_ms - last_ms_ : 0;
    if (dt > 100) dt = 100;  // 오래 못 읽었으면 크게 쌓지 않는다
    started_ = true;
    last_ms_ = now_ms;
    if (seen != cand_) {
      cand_ = seen;
      cand_since_ = now_ms;
    }
    bool gyro = rate_dps >= 0;
    bool quiet = !gyro || rate_dps < QUIET_DPS;
    if (!quiet) {
      moving_ms_ = now_ms;
      quiet_since_ = now_ms;
      angle_ += rate_dps * dt / 1000.0f;
      if (rate_dps > TURN_DPS) fast_ = true;
    } else if (now_ms - moving_ms_ > TURN_KEEP_MS) {
      angle_ = 0;
      fast_ = false;
    }
    if (cand_ < 0 || cand_ == current) {
      // 그대로이거나 애매하면 돌린 것을 잊는다 (멈춘 뒤에만, 돌리는 도중에는 지나가는 방향일 수 있다)
      if (quiet && now_ms - quiet_since_ >= SETTLE_MS) angle_ = 0, fast_ = false;
      return -1;
    }
    uint32_t quiet_for = now_ms - quiet_since_;
    uint32_t cand_for = now_ms - cand_since_;
    bool turned = gyro && fast_ && angle_ >= TURN_DEG;
    if (quiet && ((turned && quiet_for >= SETTLE_MS && cand_for >= SETTLE_MS) ||
                  (quiet_for >= SLOW_MS && cand_for >= SLOW_MS))) {
      angle_ = 0;
      fast_ = false;
      return cand_;
    }
    return -1;
  }

 private:
  bool started_ = false;
  uint32_t last_ms_ = 0;
  int cand_ = -1;
  uint32_t cand_since_ = 0;
  uint32_t moving_ms_ = 0;
  uint32_t quiet_since_ = 0;
  float angle_ = 0;
  bool fast_ = false;
};

// 들어 올림: 가속도가 기준에서 PICKUP_G 이상 벗어난 채 HITS 번(40ms 마다면 약 0.12초) 이어지면 한 번 알린다.
// 책상 진동·타자는 짧게 튀었다 돌아오므로 이어지지 않는다. 기준은 조용할 때만 천천히 따라간다.
class PickupDetect {
 public:
  static constexpr float PICKUP_G = 0.2f;
  static constexpr uint8_t HITS = 3;
  // 조용할 때 기준을 따라가는 비율
  static constexpr float FOLLOW = 0.05f;

  bool sample(float x, float y, float z) {
    if (!started_) {
      started_ = true;
      bx_ = x, by_ = y, bz_ = z;
      return false;
    }
    float dev = fabsf(x - bx_) + fabsf(y - by_) + fabsf(z - bz_);
    if (dev < PICKUP_G) {
      hits_ = 0;
      bx_ += (x - bx_) * FOLLOW, by_ += (y - by_) * FOLLOW, bz_ += (z - bz_) * FOLLOW;
      return false;
    }
    if (++hits_ < HITS) return false;
    hits_ = 0;
    bx_ = x, by_ = y, bz_ = z;  // 새 자세를 기준으로 (들고 있는 동안 계속 알리지 않게)
    return true;
  }

 private:
  bool started_ = false;
  float bx_ = 0, by_ = 0, bz_ = 0;
  uint8_t hits_ = 0;
};
