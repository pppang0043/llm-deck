#include "quick_swipe.h"
#include "standby_state.h"
#include <cassert>
#include <stdint.h>

int main() {
  QuickSwipe swipe;
  using A = QuickSwipeAction;
  // 상단 탭과 화면 본문에서 시작한 쓸기는 기존 기능으로 보낸다.
  assert(swipe.sample(100, 10, true, false) == A::None);
  assert(swipe.sample(100, 10, false, false) == A::None);
  assert(!swipe.consumed());
  swipe.sample(100, 60, true, false);
  assert(swipe.sample(100, 140, true, false) == A::None);
  swipe.sample(100, 140, false, false);
  // 아래로 쓸어 열고, 뗄 때까지 원래 버튼의 클릭을 막는다.
  swipe.sample(200, 20, true, false);
  assert(swipe.sample(205, 65, true, false) == A::Open);
  assert(swipe.consumed());
  assert(swipe.sample(205, 100, true, true) == A::None);
  assert(swipe.consumed());
  swipe.sample(205, 100, false, true);
  assert(!swipe.consumed());
  // 패널 안 위로 쓸기는 닫기. 수평 뒤로 동작과 구별한다.
  swipe.sample(100, 160, true, true);
  assert(swipe.sample(100, 110, true, true) == A::Close);
  swipe.reset();
  swipe.sample(200, 10, true, false);
  assert(swipe.sample(140, 25, true, false) == A::None);
  assert(swipe.sample(140, 100, true, false) == A::None);
  swipe.reset();
  swipe.sample(100, 55, true, false);
  assert(swipe.sample(100, 95, true, false) == A::Open);
  swipe.reset();
  // 짧게 (24px) 내려도 열린다
  swipe.sample(100, 5, true, false);
  assert(swipe.sample(100, 28, true, false) == A::None);
  assert(swipe.sample(100, 29, true, false) == A::Open);
  swipe.sample(100, 29, false, true);
  swipe.sample(100, 56, true, false);
  assert(swipe.sample(100, 120, true, false) == A::None);

  StandbyState sleep;
  assert(!sleep.active());
  sleep.enter(100);
  assert(sleep.update(100));
  assert(sleep.lower_cpu(false, false, false));
  assert(!sleep.lower_cpu(true, false, false));
  assert(!sleep.lower_cpu(false, true, false));
  assert(!sleep.lower_cpu(false, false, true));
  // 실제 터치와 display_poke(알림/버튼)는 같은 방식으로 복귀한다.
  assert(!sleep.update(101));
  assert(!sleep.lower_cpu(false, false, false));
  sleep.enter(UINT32_MAX);
  assert(!sleep.update(0)); // millis wrap 도 깨우기 신호를 놓치지 않는다
  sleep.enter(0);
  assert(sleep.active());
  assert(sleep.wake()); // BOOT 은 복귀만 하고 마이크를 바꾸지 않는다
  assert(!sleep.wake());
}
