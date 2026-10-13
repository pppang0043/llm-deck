#include "net.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiMulti.h>

#include "link.h"
#include "mic.h"
#include "secrets.h"
#include "ui.h"

#define HOSTNAME "llm-deck"

static WebServer server(80);
static WiFiMulti multi;
static Preferences prefs;
static SemaphoreHandle_t lock;
static bool station = false;
static bool was_connected = false;

// 기기에 저장된 Wi-Fi 목록: [{"s": 이름, "p": 비밀번호}, ...]
static JsonDocument saved;
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
    if (station && WiFi.status() != WL_CONNECTED && link_active() != LINK_BLE) {
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
  JsonDocument doc;
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

static void start_services() {
  MDNS.begin(HOSTNAME);
  MDNS.addService(HOSTNAME, "tcp", 80);
  server.begin();
}

void net_init() {
  server.on("/usage", HTTP_POST, handle_usage);
  server.on("/status", HTTP_GET, send_state);
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
  if (connected) server.handleClient();
}
