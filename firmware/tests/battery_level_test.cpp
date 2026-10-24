#include "battery_level.h"
#include <assert.h>

int main() {
  // 2026-10-24: 충전 중에 다시 켜면 93% 이던 것이 83% 로 떨어졌다 (충전 중 전압 4146 mV, 빼는 값 100 mV)
  {
    BatteryLevel fresh;
    int guess = fresh.update(4146, true, false, 0);
    assert(guess < 90);

    BatteryLevel level;
    level.resume(93);
    assert(level.update(4146, true, false, 0) == 93);
    // 이어 간 값에서 다시 내려가지 않는다
    assert(level.update(4146, true, false, 60000) == 93);
  }
  {
    // 끝나지 않은 충전은 99% 까지만
    BatteryLevel level;
    level.resume(100);
    assert(level.update(4146, true, false, 0) == 99);
  }
  {
    // 배터리로 켜졌으면 저장값을 쓰지 않는다 (꺼진 사이 줄었을 수 있다)
    BatteryLevel level;
    level.resume(93);
    assert(level.update(3820, false, false, 0) == 50);
  }
  {
    // 전압으로 짐작한 값이 더 높으면 그것을 쓴다
    BatteryLevel level;
    level.resume(10);
    assert(level.update(4146, true, false, 0) > 10);
  }
  {
    // 처음 한 번만 쓴다: 뽑았다 다시 꽂으면 그때 전압으로
    BatteryLevel level;
    level.resume(93);
    level.update(4146, true, false, 0);
    level.update(0, false, false, 1000);  // 배터리 없음 → 처음부터
    assert(level.update(4146, true, false, 2000) < 90);
  }
  return 0;
}
