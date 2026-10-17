#include "history.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <time.h>

#define MAX_SERIES 6
#define KEY_LEN 40
#define SAVE_MS (30 * 60 * 1000UL)
#define BLOB_VERSION 1

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

static Store *store;  // PSRAM
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

void history_init() {
  lock = xSemaphoreCreateMutex();
  store = (Store *)heap_caps_calloc(1, sizeof(Store), MALLOC_CAP_SPIRAM);
  if (!store) return;
  prefs.begin("history");
  if (prefs.getBytes("data", store, sizeof(Store)) != sizeof(Store) || store->version != BLOB_VERSION) {
    memset(store, 0, sizeof(Store));
    store->version = BLOB_VERSION;
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
  prefs.putBytes("data", store, sizeof(Store));
  xSemaphoreGive(lock);
  Serial.println("[history] saved");
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
