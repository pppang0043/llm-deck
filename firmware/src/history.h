#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

// 사용률 기록: 서비스·구간(예: "Claude Code" 의 "5시간")마다 최근 24시간을 10분 칸으로 모은다.
// 함께 최근 7일을 1시간 칸으로, 그 시간 동안 오른 사용률(%)의 합을 모은다 (주간 요약).
// 30분마다 기기에 저장해 다시 켜도 남는다. 시계가 맞춰지기 전에는 기록하지 않는다.

#define HISTORY_SLOTS 144     // 24시간 / 10분
#define HISTORY_SLOT_S 600    // 한 칸 = 10분
#define HISTORY_NONE 255      // 그 칸에 값이 없음
#define WEEK_HOURS 168        // 7일 / 1시간

void history_init();
// 화면에 보여 줄 사용량을 받을 때마다 부른다 (ui_set_usage)
void history_record(JsonArrayConst services);
// loop() 에서 부른다 (바뀐 기록을 30분마다 저장)
void history_loop();

// service 의 label 구간 기록을 오래된 것부터 out 에 HISTORY_SLOTS 개 채운다 (값이 없으면 HISTORY_NONE).
// 맨 끝 칸이 지금. 기록이 없으면 false.
bool history_get(const char *service, const char *label, uint8_t *out);

// 최근 7일: 오래된 것부터 out 에 WEEK_HOURS 개 (한 칸 = 그 1시간 동안 오른 %, 기록이 없던 시간은 HISTORY_NONE).
// 맨 끝 칸이 지금 시간. 기록이 없으면 false.
bool history_week(const char *service, const char *label, uint8_t *out);

// 지금 사용률 pct 로 보아 초기화(resets, 0 이면 모름) 전에 한도가 찰 것 같으면 그 시각, 아니면 0.
// 5시간 구간은 최근 1시간, 그 밖(주간)은 최근 6시간 동안 오른 만큼으로 본다. 지금 칸에 기록이 없으면(오래된 값) 0.
time_t history_forecast(const char *service, const char *label, int pct, uint32_t resets);

// ---- 계산만 하는 부분 (기기 밖, PC 에서도 시험할 수 있게 여기 둔다) ----

// 이보다 짧게 쌓인 기록으로는 예상하지 않는다 (10분 칸 2개)
#define FORECAST_MIN_SLOTS 2

// v[n] (맨 끝이 지금 칸) 에서 최근 lookback 칸 동안 오른 속도로 pct 가 100 이 될 때까지 남은 초. 오르지 않았으면 -1.
// 뒤로 가며 값이 오히려 크면 그 앞은 초기화 전이므로 거기서 멈춘다.
inline long history_left_s(const uint8_t *v, int n, int pct, int lookback) {
  if (pct <= 0 || pct >= 100 || n < 1 || v[n - 1] == HISTORY_NONE) return -1;
  int last = n - 1, from = -1, prev = pct;
  for (int i = last - 1; i >= 0 && last - i <= lookback; i--) {
    if (v[i] == HISTORY_NONE) continue;
    if (v[i] > prev) break;
    from = i;
    prev = v[i];
  }
  if (from < 0 || last - from < FORECAST_MIN_SLOTS || pct <= v[from]) return -1;
  uint32_t span_s = (last - from) * HISTORY_SLOT_S;
  return (long)((100 - pct) * (uint64_t)span_s / (pct - v[from]));
}

// 한 시간 칸에 새 값을 더한다: 오른 만큼만 (초기화로 떨어진 것은 무시). before 가 HISTORY_NONE 이면 처음 값.
inline void history_add_rise(uint8_t &cell, int before, int pct) {
  if (cell == HISTORY_NONE) cell = 0;
  if (before == HISTORY_NONE || pct <= before) return;
  int sum = cell + (pct - before);
  cell = sum > 254 ? 254 : sum;
}

// 7일 요약. day[] 는 오늘을 맨 끝으로 한 최근 7일(날짜별) 오른 % 의 합, day_wday[] 는 그 요일 (0 = 일요일).
struct WeekSummary {
  int day[7];
  int day_wday[7];
  int best_day;   // day[] 의 칸 번호, 쓴 것이 없으면 -1
  int best_hour;  // 가장 많이 쓴 2시간의 시작 시 (0~23), 쓴 것이 없으면 -1
  int total;
};

// h[WEEK_HOURS] 는 history_week 결과, now 는 지금 시각 (지역 시간으로 날짜와 시를 나눈다)
inline WeekSummary history_summarize(const uint8_t *h, time_t now) {
  WeekSummary s = {};
  s.best_day = s.best_hour = -1;
  int by_hour[24] = {};
  struct tm t;
  localtime_r(&now, &t);
  int keys[7];
  // 오늘부터 6일 전까지의 날짜 (정오 기준이라 서머타임에도 하루씩 넘어간다)
  for (int d = 0; d < 7; d++) {
    struct tm noon = t;
    noon.tm_hour = 12;
    noon.tm_min = noon.tm_sec = 0;
    noon.tm_mday -= 6 - d;
    noon.tm_isdst = -1;
    time_t at = mktime(&noon);
    struct tm day;
    localtime_r(&at, &day);
    keys[d] = (day.tm_year + 1900) * 400 + day.tm_yday;
    s.day_wday[d] = day.tm_wday;
  }
  time_t hour0 = now / 3600 * 3600 - (time_t)(WEEK_HOURS - 1) * 3600;
  for (int i = 0; i < WEEK_HOURS; i++) {
    if (h[i] == HISTORY_NONE || h[i] == 0) continue;
    time_t at = hour0 + (time_t)i * 3600;
    struct tm ht;
    localtime_r(&at, &ht);
    by_hour[ht.tm_hour] += h[i];
    s.total += h[i];
    int key = (ht.tm_year + 1900) * 400 + ht.tm_yday;
    for (int d = 0; d < 7; d++) {
      if (keys[d] == key) s.day[d] += h[i];
    }
  }
  if (s.total == 0) return s;
  for (int d = 0; d < 7; d++) {
    if (s.day[d] > 0 && (s.best_day < 0 || s.day[d] > s.day[s.best_day])) s.best_day = d;
  }
  int best = 0;
  for (int hr = 0; hr < 24; hr++) {
    int sum = by_hour[hr] + by_hour[(hr + 1) % 24];
    // 같으면 시작하는 시에 쓴 것이 많은 쪽 (20시에만 썼으면 19~21시 대신 20~22시)
    if (sum > best || (sum == best && sum > 0 && by_hour[hr] > by_hour[s.best_hour])) {
      best = sum;
      s.best_hour = hr;
    }
  }
  return s;
}
