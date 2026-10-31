#include "motion_logic.h"
#include <cassert>
#include <stdint.h>

// 40ms 마다 같은 값을 n 번 넣고, 처음으로 바꾸라고 한 방향과 그때까지 걸린 시간을 돌려준다
static uint32_t t = 0;
static int feed(TurnSettle &s, int n, float rate, int seen, int current, uint32_t *after = nullptr) {
  uint32_t start = t;
  for (int i = 0; i < n; i++) {
    t += 40;
    int r = s.sample(t, rate, seen, current);
    if (r >= 0) {
      if (after) *after = t - start;
      return r;
    }
  }
  return -1;
}

int main() {
  // 빠른 판단: 0.4초 동안 250도/초로 돌리고(100도) 멈추면 0.2초 안에 바꾼다
  {
    TurnSettle s;
    t = 0;
    assert(feed(s, 10, 2, 0, 0) == -1);
    assert(feed(s, 5, 250, -1, 0) == -1);
    assert(feed(s, 5, 250, 1, 0) == -1);  // 도는 중에는 바꾸지 않는다
    uint32_t after = 0;
    assert(feed(s, 20, 3, 1, 0, &after) == 1);
    assert(after <= 240);
  }
  // 걷기·흔들기: 자이로가 계속 움직이면 방향이 달라도 바꾸지 않는다
  {
    TurnSettle s;
    t = 0;
    for (int i = 0; i < 100; i++) {
      assert(s.sample(t += 40, i % 3 ? 10 : 60, 1, 0) == -1);
    }
  }
  // 느린 판단: 천천히 기울였으면(자이로 조용) 1초 뒤에 바꾼다
  {
    TurnSettle s;
    t = 0;
    uint32_t after = 0;
    assert(feed(s, 40, 5, 3, 0, &after) == 3);
    assert(after >= 1000 && after <= 1080);
  }
  // 자이로가 없으면 예전처럼 같은 방향 1초
  {
    TurnSettle s;
    t = 0;
    uint32_t after = 0;
    assert(feed(s, 40, -1, 2, 0, &after) == 2);
    assert(after >= 1000 && after <= 1080);
  }
  // 크게 돌렸다가 원래 방향으로 돌아오면 바꾸지 않고, 뒤늦게 다른 방향이 되어도 1초 규칙을 따른다
  {
    TurnSettle s;
    t = 0;
    feed(s, 5, 2, 0, 0);
    feed(s, 6, 300, 1, 0);
    assert(feed(s, 10, 2, 0, 0) == -1);
    uint32_t after = 0;
    assert(feed(s, 40, 2, 1, 0, &after) == 1);
    assert(after >= 1000);
  }
  // 돌린 뒤 눕혀 놓으면(애매) 그대로
  {
    TurnSettle s;
    t = 0;
    feed(s, 6, 300, 1, 0);
    assert(feed(s, 50, 1, -1, 0) == -1);
  }

  // 들어 올림
  {
    PickupDetect p;
    assert(!p.sample(0, 0, 1));
    // 책상 진동: 한두 번 튀는 것은 무시
    for (int i = 0; i < 50; i++) {
      float bump = i % 5 == 0 ? 0.4f : 0.02f;
      assert(!p.sample(bump, 0, 1));
    }
    assert(!p.sample(0, 0.5f, 0.9f));
    assert(!p.sample(0, 0.5f, 0.85f));  // 두 번만이면 아직
    assert(!p.sample(0, 0, 1));
    // 들어 올려 기울이면 세 번째에 한 번 알린다
    assert(!p.sample(0, 0.4f, 0.9f));
    assert(!p.sample(0, 0.45f, 0.88f));
    assert(p.sample(0, 0.5f, 0.86f));
    // 그 자세로 들고 있으면 다시 알리지 않는다
    for (int i = 0; i < 20; i++) assert(!p.sample(0, 0.5f, 0.86f));
    // 아주 천천히 기울어지는 것은 기준이 따라간다
    float y = 0.5f;
    for (int i = 0; i < 200; i++) assert(!p.sample(0, y += 0.003f, 0.86f));
  }
  return 0;
}
