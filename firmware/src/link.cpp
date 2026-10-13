#include "link.h"

#include <ArduinoJson.h>
#include <string>

#include "ui.h"

// 이 시간 동안 PC 에서 소식이 없으면 연결이 끊긴 것으로 본다
#define STALE_MS 120000

static SemaphoreHandle_t lock;
static std::string pending;
static Link pending_from = LINK_NONE;

static volatile Link active = LINK_NONE;
static uint32_t last_ms = 0;
static void (*set_mic_cb)(bool);

static const char *link_name(Link link) {
  switch (link) {
    case LINK_USB: return "USB";
    case LINK_BLE: return "Bluetooth";
    case LINK_WIFI: return "Wi-Fi";
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
      if (doc["services"].is<JsonArrayConst>()) {
        JsonArrayConst services = doc["services"];
        ui_set_usage(services);
        Serial.printf("[link] usage via %s: %u services\n", link_name(from), (unsigned)services.size());
      }
    }
  }

  if (active != LINK_NONE && millis() - last_ms > STALE_MS) set_active(LINK_NONE);
}

Link link_active() {
  return active;
}
