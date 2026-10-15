#include "link.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <sys/time.h>
#include <string>

#include <WiFiUdp.h>

#include "psram_json.h"
#include "battery.h"
#include "ble_link.h"
#include "cloud.h"
#include <img_converters.h>
#include <lvgl.h>

#include "camera.h"
#include "camera_ui.h"
#include "mic.h"
#include "photo.h"
#include "usb_link.h"
#include "net.h"
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
    case LINK_BLE: return "블루투스";
    case LINK_WIFI: return "Wi-Fi";
    case LINK_CLOUD: return "클라우드";
    default: return "PC 없음";
  }
}

// PC 도우미가 알려준 시각으로 시계를 맞춘다. Wi-Fi 가 없는 곳(블루투스만 쓰는 곳)에서도 시계가 나오게 한다.
static void set_clock(int64_t now, int32_t utc_offset) {
  // 시간대 표기는 부호가 반대다: 한국(UTC+9)은 "UTC-9"
  char tz[24];
  int minutes = abs(utc_offset) / 60;
  snprintf(tz, sizeof(tz), "UTC%c%d:%02d", utc_offset >= 0 ? '-' : '+', minutes / 60, minutes % 60);
  setenv("TZ", tz, 1);
  tzset();
  // 이미 거의 맞으면 건드리지 않는다
  if (llabs((int64_t)time(nullptr) - now) < 2) return;
  struct timeval tv = {(time_t)now, 0};
  settimeofday(&tv, nullptr);
}

// 로그용 영어 이름
static const char *link_tag(Link link) {
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
  Serial.printf("[link] active: %s\n", link_tag(link));
}

void link_init(void (*set_mic)(bool on)) {
  set_mic_cb = set_mic;
  lock = xSemaphoreCreateMutex();
  ui_set_status(link_name(LINK_NONE));

  prefs.begin("llm-deck");
  saved = prefs.getString("usage", "").c_str();
  shown = saved;
  JsonDocument doc(PSRAM_JSON);
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
    JsonDocument doc(PSRAM_JSON);
    if (deserializeJson(doc, message)) {
      Serial.printf("[link] bad json from %s (%u bytes)\n", link_tag(from), (unsigned)message.size());
    } else {
      last_ms = millis();
      set_active(from);
      if (doc["now"].is<int64_t>()) set_clock(doc["now"], doc["utc_offset"] | 32400);
      if (doc["mic"].is<bool>()) set_mic_cb(doc["mic"]);
      if (doc["battery_mv"].is<int>()) battery_calibrate(doc["battery_mv"]);
      if (doc["camera"].is<const char *>()) camera_ui_command(doc["camera"]);
      if (doc["view"].is<const char *>()) ui_show(doc["view"]);
      if (doc["screenshot"] == true) {
        // 진단용: 지금 화면을 사진처럼 PC 로 보낸다
        uint16_t *px;
        int w, h;
        lv_refr_now(NULL);
        if (ui_screenshot(&px, &w, &h)) {
          for (int i = 0; i < w * h; i++) px[i] = (px[i] << 8) | (px[i] >> 8);
          uint8_t *jpg;
          size_t len;
          if (fmt2jpg((uint8_t *)px, w * h * 2, w, h, PIXFORMAT_RGB565, 90, &jpg, &len)) photo_save(jpg, len);
          free(px);
        }
      }
      if (doc["camera_mount"].is<JsonObjectConst>()) {
        camera_set_mount(doc["camera_mount"]["rot"] | 0, doc["camera_mount"]["mirror"] | false);
      }
      if (doc["photo_test"] == true) {
        uint8_t *jpg;
        size_t len;
        if (camera_test_pattern(&jpg, &len)) photo_save(jpg, len);
      }
      if (doc["sync_url"].is<const char *>()) cloud_set_url(doc["sync_url"]);
      if (doc["wifi_add"]["ssid"].is<const char *>()) net_add_wifi(doc["wifi_add"]["ssid"], doc["wifi_add"]["password"] | "");
      if (doc["wifi_clear"] | false) net_clear_wifi();
      if (doc["services"].is<JsonArrayConst>()) {
        JsonArrayConst services = doc["services"];
        ui_set_usage(services);
        shown.clear();
        serializeJson(services, shown);
        Serial.printf("[link] usage via %s: %u services\n", link_tag(from), (unsigned)services.size());
      }
    }
  }

  save_if_due();
  if (active != LINK_NONE && millis() - last_ms > STALE_MS) set_active(LINK_NONE);
}

Link link_active() {
  return active;
}

void link_send_event(const char *json) {
  static WiFiUDP udp;
  switch (active) {
    case LINK_USB:
      usb_send_event(json);
      break;
    case LINK_BLE:
      ble_send_event(json);
      break;
    case LINK_WIFI:
      if ((uint32_t)mic_target() != 0) {
        udp.beginPacket(mic_target(), MIC_UDP_PORT);
        udp.write((const uint8_t *)json, strlen(json));
        udp.endPacket();
      }
      break;
    default:
      return;
  }
  Serial.printf("[link] event to %s: %s\n", link_tag(active), json);
}
