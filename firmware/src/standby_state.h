#pragma once
#include <stdint.h>

// 수동 절전은 설정을 바꾸거나 재부팅 뒤에 복원하지 않는다. 기존 깨우기 신호(display_poke)도 따른다.
class StandbyState {
 public:
  void enter(uint32_t wake_sequence) { active_ = true; sequence_ = wake_sequence; }
  bool update(uint32_t wake_sequence) {
    if (active_ && wake_sequence != sequence_) active_ = false;
    return active_;
  }
  bool active() const { return active_; }
  bool wake() { bool was = active_; active_ = false; return was; }
  bool lower_cpu(bool camera, bool transfer, bool audio) const {
    return active_ && !camera && !transfer && !audio;
  }
 private:
  bool active_ = false;
  uint32_t sequence_ = 0;
};
