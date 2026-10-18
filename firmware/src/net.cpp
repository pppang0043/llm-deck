#include "net.h"

#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiMulti.h>

#include "psram_json.h"
#include "health.h"
#include "link.h"
#include "mic.h"
#include "photo.h"
#include "settings.h"
#include "secrets.h"
#include "ui.h"

#define HOSTNAME "llm-deck"

static WebServer server(80);
static volatile bool radio_on = true;
static WiFiMulti multi;
static Preferences prefs;
static SemaphoreHandle_t lock;
static bool station = false;
static bool was_connected = false;

// 기기에 저장된 Wi-Fi 목록: [{"s": 이름, "p": 비밀번호}, ...]
static JsonDocument saved(PSRAM_JSON);
// 메인 루프에서 추가한 Wi-Fi 를 접속 태스크로 넘기는 칸
static char pending_ssid[33] = "";
static char pending_password[65] = "";

// 여러 Wi-Fi 중 잡히는 곳을 찾는 일은 몇 초씩 걸려서, 화면이 멈추지 않도록 따로 돌린다
static void wifi_task(void *) {
  for (;;) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (pending_ssid[0]) {
      multi.addAP(pending_ssid, pending_password);
      pending_ssid[0] = 0;
      station = true;
    }
    xSemaphoreGive(lock);
    // Wi-Fi 를 찾는 동안에는 같은 안테나를 쓰는 블루투스가 끊길 수 있다.
    // 블루투스로 PC 와 연결되어 있으면 찾지 않고, 못 찾았을 때는 한동안 쉬었다가 다시 찾는다.
    bool searched = false;
    if (radio_on && station && WiFi.status() != WL_CONNECTED && link_active() != LINK_BLE) {
      searched = multi.run(8000) != WL_CONNECTED;
    }
    vTaskDelay(pdMS_TO_TICKS(searched ? 30000 : 3000));
  }
}

static void store_saved() {
  String out;
  serializeJson(saved, out);
  prefs.putString("wifi", out);
}

static void send_state() {
  JsonDocument doc(PSRAM_JSON);
  doc["ok"] = true;
  doc["mic"] = mic_is_on();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handle_usage() {
  const String &body = server.arg("plain");
  mic_set_target(server.client().remoteIP());
  link_submit(LINK_WIFI, body.c_str(), body.length());
  send_state();
}

// 무선 펌웨어 업로드 (PC 에서: pio run -e ota -t upload). 비밀번호가 맞아야 받는다.
static bool ota_ready = false;

static void start_ota() {
  if (ota_ready || strlen(OTA_PASSWORD) == 0) return;
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setMdnsEnabled(false);  // mDNS 는 위에서 이미 켰다
  ArduinoOTA.onStart([] {
    Serial.println("[ota] update started");
    ui_toast("무선으로 펌웨어를\n업데이트하는 중...", 120000);
  });
  ArduinoOTA.onEnd([] { Serial.println("[ota] done, restarting"); });
  // 받는 동안은 loop() 로 돌아오지 않으므로 받은 만큼 감시 장치에 알린다 (끊기면 한계시간 뒤 다시 켜진다)
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { health_watch_feed(); });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[ota] failed: %d\n", (int)e);
    ui_toast("무선 업데이트 실패", 3000);
  });
  ArduinoOTA.begin();
  ota_ready = true;
  Serial.println("[ota] ready");
}

static void start_services() {
  MDNS.begin(HOSTNAME);
  MDNS.addService(HOSTNAME, "tcp", 80);
  // espota 가 찾을 수 있게 알린다
  if (strlen(OTA_PASSWORD) > 0) MDNS.enableArduino(3232, true);
  server.begin();
  start_ota();
}

void net_init() {
  server.on("/usage", HTTP_POST, handle_usage);
  server.on("/status", HTTP_GET, send_state);
  // 마지막으로 찍은 사진. 사진을 찍으면 PC 에 알림을 보내고, PC 가 여기서 받아 간다.
  server.on("/photo", HTTP_GET, [] {
    const uint8_t *data;
    size_t len;
    const char *name;
    if (!photo_last(&data, &len, &name)) {
      server.send(404, "text/plain", "no photo");
      return;
    }
    server.sendHeader("Content-Disposition", String("inline; filename=") + name);
    server.send_P(200, "image/jpeg", (const char *)data, len);
  });
  WiFi.setHostname(HOSTNAME);

  lock = xSemaphoreCreateMutex();
  prefs.begin("llm-deck");
  deserializeJson(saved, prefs.getString("wifi", "[]"));
  if (!saved.is<JsonArray>()) saved.to<JsonArray>();

  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_SSID) > 0) {
    multi.addAP(WIFI_SSID, WIFI_PASSWORD);
    station = true;
  }
  for (JsonObjectConst wifi : saved.as<JsonArrayConst>()) {
    multi.addAP(wifi["s"] | "", wifi["p"] | "");
    station = true;
  }
  Serial.printf("[net] %u saved wifi networks%s\n", (unsigned)saved.size(), strlen(WIFI_SSID) > 0 ? " + secrets.h" : "");
  radio_on = settings_link_allowed(LINK_WIFI) || settings_link_allowed(LINK_CLOUD);
  if (!radio_on) Serial.println("[net] wifi off in settings");
  ui_set_wifi_info(station ? "연결 중..." : "설정 안 됨");
  xTaskCreatePinnedToCore(wifi_task, "wifi", 4096, NULL, 1, NULL, 1);
}

void net_add_wifi(const char *ssid, const char *password) {
  if (!ssid[0] || strlen(ssid) >= sizeof(pending_ssid) || strlen(password) >= sizeof(pending_password)) return;
  JsonArray list = saved.as<JsonArray>();
  for (size_t i = 0; i < list.size(); i++) {
    if (strcmp(list[i]["s"] | "", ssid) == 0) list.remove(i--);
  }
  while (list.size() >= NET_MAX_SAVED) list.remove(0);
  JsonObject wifi = list.add<JsonObject>();
  wifi["s"] = ssid;
  wifi["p"] = password;
  store_saved();

  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(pending_ssid, ssid, sizeof(pending_ssid));
  strlcpy(pending_password, password, sizeof(pending_password));
  xSemaphoreGive(lock);
  Serial.printf("[net] wifi added: %s (%u saved)\n", ssid, (unsigned)list.size());
}

String net_saved_names() {
  String out;
  if (strlen(WIFI_SSID) > 0) out = WIFI_SSID;
  for (JsonObjectConst wifi : saved.as<JsonArrayConst>()) {
    const char *name = wifi["s"] | "";
    if (strcmp(name, WIFI_SSID) == 0) continue;
    if (out.length()) out += "\n";
    out += name;
  }
  return out;
}

int net_added_count() {
  return saved.size();
}

void net_clear_wifi() {
  saved.to<JsonArray>();
  store_saved();
  Serial.println("[net] saved wifi cleared (restart to apply)");
}

void net_loop() {
  if (!station) return;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !was_connected) {
    Serial.printf("[net] connected, ip %s\n", WiFi.localIP().toString().c_str());
    start_services();
    ui_set_wifi_info(WiFi.localIP().toString().c_str());
  } else if (!connected && was_connected) {
    Serial.println("[net] wifi lost");
    ui_set_wifi_info("다시 연결 중...");
  }
  was_connected = connected;
  if (connected) {
    server.handleClient();
    if (ota_ready) ArduinoOTA.handle();
  }
}

void net_set_enabled(bool on) {
  if (on == radio_on) return;
  radio_on = on;
  if (!on) {
    WiFi.disconnect(true);
    Serial.println("[net] wifi off in settings");
    ui_set_wifi_info("꺼짐 (설정)");
  } else {
    WiFi.mode(WIFI_STA);
    Serial.println("[net] wifi on");
    ui_set_wifi_info("연결 중...");
  }
}
