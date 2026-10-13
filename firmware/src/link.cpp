#include "link.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <string>

#include "cloud.h"
#include "ui.h"

// 이 시간 동안 PC 에서 소식이 없으면 연결이 끊긴 것으로 본다
#define STALE_MS 120000
// 받은 사용량을 플래시에 저장하는 최소 간격. 플래시는 쓰기 횟수에 수명이 있어 자주 쓰지 않는다.
#define SAVE_INTERVAL_MS 300000
// 플래시(NVS)에 저장할 수 있는 글자 수 한계
#define SAVE_MAX_BYTES 3900

static SemaphoreHandle_t lock;
static std::string pending;
static Link pending_from = LINK_NONE;

static volatile Link active = LINK_NONE;
static uint32_t last_ms = 0;
static void (*set_mic_cb)(bool);

// 마지막으로 받은 사용량. PC 없이 전원만 연결해도 이 값을 보여준다.
static Preferences prefs;
static std::string shown;
static std::string saved;
static uint32_t saved_ms = 0;

static void save_if_due() {
  if (shown == saved || shown.size() > SAVE_MAX_BYTES) return;
  if (!saved.empty() && millis() - saved_ms < SAVE_INTERVAL_MS) return;
  prefs.putString("usage", shown.c_str());
  saved = shown;
  saved_ms = millis();
  Serial.printf("[link] usage saved (%u bytes)\n", (unsigned)saved.size());
}

static const char *link_name(Link link) {
  switch (link) {
    case LINK_USB: return "USB";
    case LINK_BLE: return "Bluetooth";
    case LINK_WIFI: return "Wi-Fi";
    case LINK_CLOUD: return "Cloud";
    default: return "No PC";
  }
}

static void set_active(Link link) {
  if (link == active) return;
  active = link;
  ui_set_status(link_name(link));
  Serial.printf("[link] active: %s\n", link_name(link));
}

void link_init(void (*set_mic)(bool on)) {
  set_mic_cb = set_mic;
  lock = xSemaphoreCreateMutex();
  ui_set_status(link_name(LINK_NONE));

  prefs.begin("llm-deck");
  saved = prefs.getString("usage", "").c_str();
  shown = saved;
  JsonDocument doc;
  if (!saved.empty() && !deserializeJson(doc, saved)) {
    ui_set_usage(doc.as<JsonArrayConst>());
    Serial.printf("[link] restored saved usage: %u services\n", (unsigned)doc.size());
  }
}

void link_submit(Link from, const char *data, size_t len) {
  xSemaphoreTake(lock, portMAX_DELAY);
  pending.assign(data, len);
  pending_from = from;
  xSemaphoreGive(lock);
}

void link_loop() {
  std::string message;
  Link from;
  xSemaphoreTake(lock, portMAX_DELAY);
  message.swap(pending);
  from = pending_from;
  xSemaphoreGive(lock);

  if (!message.empty()) {
    JsonDocument doc;
    if (deserializeJson(doc, message)) {
      Serial.printf("[link] bad json from %s (%u bytes)\n", link_name(from), (unsigned)message.size());
    } else {
      last_ms = millis();
      set_active(from);
      if (doc["mic"].is<bool>()) set_mic_cb(doc["mic"]);
      if (doc["sync_url"].is<const char *>()) cloud_set_url(doc["sync_url"]);
      if (doc["services"].is<JsonArrayConst>()) {
        JsonArrayConst services = doc["services"];
        ui_set_usage(services);
        shown.clear();
        serializeJson(services, shown);
        Serial.printf("[link] usage via %s: %u services\n", link_name(from), (unsigned)services.size());
      }
    }
  }

  save_if_due();
  if (active != LINK_NONE && millis() - last_ms > STALE_MS) set_active(LINK_NONE);
}

Link link_active() {
  return active;
}
