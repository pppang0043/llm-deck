#pragma once

#include <ArduinoJson.h>
#include <stdlib.h>
#include <time.h>

// 한도 알림: 새 사용량을 받을 때마다 지난번 값과 비교해, 사용률이 80% 나 90% 를 넘었거나
// 한도가 초기화되었으면 화면에 알림을 띄우고 소리를 낸다 (블루투스 스피커가 있으면 그쪽, 없으면 PC 스피커).
// 또 지금 속도면 곧 소진될 것 같을 때(5시간 구간은 1시간 안, 주간은 12시간 안, 초기화 전) 미리 한 번 알린다.
// 처음 보는 구간(켠 직후 등)은 비교할 값이 없으므로 알리지 않는다.
void alerts_check(JsonArrayConst services);

// ---- 미리 알림 판단 (계산만, PC 에서도 시험할 수 있게 여기 둔다) ----

#define PREALERT_QUIET_S (30 * 60)  // 알린 뒤 예상이 이만큼 사라졌다가 돌아와야 다시 볼 수 있다

// 서비스·구간마다 하나
struct PreAlert {
  uint32_t resets = 0;     // 지금 구간의 초기화 시각 (구간이 바뀌었는지 보려고)
  long alerted_left = -1;  // 알릴 때 남았던 초. -1 이면 이번 구간에 아직 안 알림
  time_t clear_since = 0;  // 알린 뒤 예상이 사라진 시각 (0 이면 예상이 이어지는 중)
};

// left: 지금 속도면 소진까지 남은 초 (-1 이면 예상 없음), limit: 이보다 짧으면 알림.
// dropped: 사용률이 크게 떨어졌다 (초기화). busy: 이번에는 다른 알림이 소리를 쓴다 (알리지 않고 다음으로 미룸).
// 구간마다 한 번만. 다만 예상이 30분 넘게 사라졌다가 처음 알린 때의 절반 이하로 빨라져 돌아오면 다시 알린다.
inline bool prealert_step(PreAlert &st, time_t now, uint32_t resets, bool dropped, long left, long limit, bool busy) {
  bool new_window = dropped || (resets && st.resets && labs((long)resets - (long)st.resets) > 1800);
  if (new_window) {
    st.alerted_left = -1;
    st.clear_since = 0;
  }
  if (resets) st.resets = resets;
  bool soon = left >= 0 && left <= limit;
  if (!soon) {
    if (st.alerted_left >= 0 && !st.clear_since) st.clear_since = now;
    return false;
  }
  if (st.alerted_left < 0) {
    if (busy) return false;
    st.alerted_left = left;
    st.clear_since = 0;
    return true;
  }
  bool quiet_long = st.clear_since && now - st.clear_since >= PREALERT_QUIET_S;
  if (quiet_long && left <= st.alerted_left / 2) {
    if (busy) return false;
    st.alerted_left = left;
    st.clear_since = 0;
    return true;
  }
  // 잠깐 사라졌다 돌아온 것(들쭉날쭉)은 조용히, 조용한 시간도 처음부터
  st.clear_since = 0;
  return false;
}
