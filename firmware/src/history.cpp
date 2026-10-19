#include "history.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <time.h>

#define MAX_SERIES 6
#define KEY_LEN 40
#define SAVE_MS (30 * 60 * 1000UL)
#define BLOB_VERSION 1
#define WEEK_VERSION 1

// 한 구간의 기록. v 는 원형으로 쓴다: 칸 번호(시각/10분) % HISTORY_SLOTS 자리에 그 칸의 값.
struct Series {
  char key[KEY_LEN];  // "서비스\t구간"
  uint32_t last_slot; // 마지막으로 쓴 칸 번호 (0 이면 빈 자리)
  uint8_t v[HISTORY_SLOTS];
};

struct Store {
  uint8_t version;
  Series series[MAX_SERIES];
};

// 7일 기록 (따로 저장해 예전 24시간 기록은 그대로 읽힌다). v 는 시간 번호(시각/1시간) % WEEK_HOURS 자리.
struct WeekSeries {
  char key[KEY_LEN];
  uint32_t last_hour;  // 마지막으로 쓴 시간 번호 (0 이면 빈 자리)
  uint8_t last_pct;    // 마지막으로 받은 사용률 (오른 만큼을 더하려고)
  uint8_t v[WEEK_HOURS];
};

struct WeekStore {
  uint8_t version;
  WeekSeries series[MAX_SERIES];
};

static Store *store;  // PSRAM
static WeekStore *week;  // PSRAM
static Preferences prefs;
static bool dirty = false;
static SemaphoreHandle_t lock;

static void make_key(char *out, const char *service, const char *label) {
  snprintf(out, KEY_LEN, "%s\t%s", service, label);
}

static Series *find(const char *key, bool create) {
  Series *empty = nullptr, *oldest = nullptr;
  for (Series &s : store->series) {
    if (s.last_slot && strcmp(s.key, key) == 0) return &s;
    if (!s.last_slot && !empty) empty = &s;
    if (!oldest || s.last_slot < oldest->last_slot) oldest = &s;
  }
  if (!create) return nullptr;
  // 자리가 없으면 가장 오래 안 쓴 구간을 지운다 (서비스가 바뀌었을 때)
  Series *s = empty ? empty : oldest;
  strlcpy(s->key, key, sizeof(s->key));
  s->last_slot = 0;
  memset(s->v, HISTORY_NONE, sizeof(s->v));
  return s;
}

static WeekSeries *find_week(const char *key, bool create) {
  WeekSeries *empty = nullptr, *oldest = nullptr;
  for (WeekSeries &w : week->series) {
    if (w.last_hour && strcmp(w.key, key) == 0) return &w;
    if (!w.last_hour && !empty) empty = &w;
    if (!oldest || w.last_hour < oldest->last_hour) oldest = &w;
  }
  if (!create) return nullptr;
  WeekSeries *w = empty ? empty : oldest;
  strlcpy(w->key, key, sizeof(w->key));
  w->last_hour = 0;
  w->last_pct = HISTORY_NONE;
  memset(w->v, HISTORY_NONE, sizeof(w->v));
  return w;
}

// w 에 지금(hour) 사용률 pct 를 더한다. 건너뛴 시간은 비워 둔다.
static void week_add(WeekSeries *w, uint32_t hour, int pct) {
  if (w->last_hour && hour < w->last_hour) return;
  if (hour != w->last_hour) {
    if (w->last_hour) {
      uint32_t gap = hour - w->last_hour;
      if (gap > WEEK_HOURS) gap = WEEK_HOURS;
      for (uint32_t i = 1; i < gap; i++) w->v[(w->last_hour + i) % WEEK_HOURS] = HISTORY_NONE;
    }
    w->v[hour % WEEK_HOURS] = HISTORY_NONE;
    w->last_hour = hour;
  }
  history_add_rise(w->v[hour % WEEK_HOURS], w->last_pct, pct);
  w->last_pct = pct;
}

// 7일 기록이 처음 생길 때(이 기능 전 펌웨어에서 올린 뒤) 24시간 기록으로 채워 둔다
static void week_from_day() {
  for (Series &s : store->series) {
    if (!s.last_slot) continue;
    WeekSeries *w = find_week(s.key, true);
    uint32_t first = s.last_slot >= HISTORY_SLOTS - 1 ? s.last_slot - (HISTORY_SLOTS - 1) : 0;
    for (uint32_t slot = first; slot <= s.last_slot; slot++) {
      uint8_t pct = s.v[slot % HISTORY_SLOTS];
      if (pct != HISTORY_NONE) week_add(w, slot * HISTORY_SLOT_S / 3600, pct);
    }
  }
}

void history_init() {
  lock = xSemaphoreCreateMutex();
  store = (Store *)heap_caps_calloc(1, sizeof(Store), MALLOC_CAP_SPIRAM);
  if (!store) return;
  prefs.begin("history");
  if (prefs.getBytes("data", store, sizeof(Store)) != sizeof(Store) || store->version != BLOB_VERSION) {
    memset(store, 0, sizeof(Store));
    store->version = BLOB_VERSION;
  }
  week = (WeekStore *)heap_caps_calloc(1, sizeof(WeekStore), MALLOC_CAP_SPIRAM);
  if (!week) return;
  if (prefs.getBytes("week", week, sizeof(WeekStore)) != sizeof(WeekStore) || week->version != WEEK_VERSION) {
    memset(week, 0, sizeof(WeekStore));
    week->version = WEEK_VERSION;
    week_from_day();
    dirty = true;
  }
}

void history_record(JsonArrayConst services) {
  time_t now = time(nullptr);
  if (!store || now < 1700000000) return;
  uint32_t slot = now / HISTORY_SLOT_S;
  xSemaphoreTake(lock, portMAX_DELAY);
  for (JsonObjectConst svc : services) {
    const char *name = svc["name"] | "";
    for (JsonObjectConst bar : svc["bars"].as<JsonArrayConst>()) {
      int pct = bar["pct"] | -1;
      if (pct < 0 || !name[0]) continue;
      char key[KEY_LEN];
      make_key(key, name, bar["label"] | "");
      Series *s = find(key, true);
      // 건너뛴 칸(기기가 꺼져 있었거나 값이 안 왔을 때)은 비워 둔다
      if (s->last_slot && slot > s->last_slot) {
        uint32_t gap = slot - s->last_slot;
        if (gap > HISTORY_SLOTS) gap = HISTORY_SLOTS;
        for (uint32_t i = 1; i < gap; i++) s->v[(s->last_slot + i) % HISTORY_SLOTS] = HISTORY_NONE;
      }
      if (slot >= s->last_slot) {
        s->v[slot % HISTORY_SLOTS] = constrain(pct, 0, 100);
        s->last_slot = slot;
        dirty = true;
      }
      if (week) week_add(find_week(key, true), now / 3600, constrain(pct, 0, 100));
    }
  }
  xSemaphoreGive(lock);
}

void history_loop() {
  static uint32_t saved_ms = 0;
  if (!store || !dirty || millis() - saved_ms < SAVE_MS) return;
  saved_ms = millis();
  xSemaphoreTake(lock, portMAX_DELAY);
  dirty = false;
  bool ok = prefs.putBytes("data", store, sizeof(Store)) == sizeof(Store);
  if (week) ok = prefs.putBytes("week", week, sizeof(WeekStore)) == sizeof(WeekStore) && ok;
  xSemaphoreGive(lock);
  Serial.println(ok ? "[history] saved" : "[history] save failed (NVS full?)");
}

bool history_get(const char *service, const char *label, uint8_t *out) {
  if (!store) return false;
  char key[KEY_LEN];
  make_key(key, service, label);
  uint32_t now_slot = (uint32_t)(time(nullptr) / HISTORY_SLOT_S);
  xSemaphoreTake(lock, portMAX_DELAY);
  Series *s = find(key, false);
  if (!s) {
    xSemaphoreGive(lock);
    return false;
  }
  // 맨 끝이 지금 칸이 되도록 펼친다. 마지막 기록 뒤로 지난 칸은 비어 있다.
  for (int i = 0; i < HISTORY_SLOTS; i++) {
    uint32_t slot = now_slot - (HISTORY_SLOTS - 1) + i;
    bool known = slot <= s->last_slot && s->last_slot - slot < HISTORY_SLOTS;
    out[i] = known ? s->v[slot % HISTORY_SLOTS] : HISTORY_NONE;
  }
  xSemaphoreGive(lock);
  return true;
}

bool history_week(const char *service, const char *label, uint8_t *out) {
  if (!week) return false;
  char key[KEY_LEN];
  make_key(key, service, label);
  uint32_t now_hour = (uint32_t)(time(nullptr) / 3600);
  xSemaphoreTake(lock, portMAX_DELAY);
  WeekSeries *w = find_week(key, false);
  if (!w) {
    xSemaphoreGive(lock);
    return false;
  }
  for (int i = 0; i < WEEK_HOURS; i++) {
    uint32_t hour = now_hour - (WEEK_HOURS - 1) + i;
    bool known = hour <= w->last_hour && w->last_hour - hour < WEEK_HOURS;
    out[i] = known ? w->v[hour % WEEK_HOURS] : HISTORY_NONE;
  }
  xSemaphoreGive(lock);
  return true;
}

time_t history_forecast(const char *service, const char *label, int pct, uint32_t resets) {
  time_t now = time(nullptr);
  if (pct <= 0 || pct >= 100 || now < 1700000000) return 0;
  uint8_t v[HISTORY_SLOTS];
  if (!history_get(service, label, v)) return 0;
  // 5시간 구간은 최근 1시간(6칸), 그 밖(주간)은 최근 6시간(36칸)
  int lookback = strcmp(label, "5시간") == 0 ? 6 : 36;
  long left = history_left_s(v, HISTORY_SLOTS, pct, lookback);
  if (left < 0 || left > 7 * 86400) return 0;
  time_t full = now + left;
  if (resets && full >= (time_t)resets) return 0;
  return full;
}
