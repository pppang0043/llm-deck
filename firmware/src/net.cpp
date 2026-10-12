#include "net.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "mic.h"
#include "secrets.h"
#include "ui.h"

#define HOSTNAME "llm-deck"
// 이 시간 동안 PC 에서 소식이 없으면 상태줄에 표시한다
#define STALE_MS 120000

static WebServer server(80);
static bool was_connected = false;
static bool configured = false;
static uint32_t last_usage_ms = 0;
static bool stale_shown = false;

static void show_ip() {
  ui_set_status(WiFi.localIP().toString().c_str());
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
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  mic_set_target(server.client().remoteIP());
  ui_set_usage(doc["services"].as<JsonArrayConst>(), doc["updated"]);
  last_usage_ms = millis();
  if (stale_shown) {
    stale_shown = false;
    show_ip();
  }
  send_state();
}

void net_init() {
  configured = strlen(WIFI_SSID) > 0;
  if (!configured) {
    Serial.println("[net] WIFI_SSID is empty - fill in include/secrets.h");
    ui_set_status("Wi-Fi not set");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  // 절전 모드를 끄면 마이크 오디오가 끊기지 않는다
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  ui_set_status("Wi-Fi connecting");

  server.on("/usage", HTTP_POST, handle_usage);
  server.on("/status", HTTP_GET, send_state);
}

void net_loop() {
  if (!configured) return;

  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !was_connected) {
    Serial.printf("[net] connected, ip %s\n", WiFi.localIP().toString().c_str());
    MDNS.begin(HOSTNAME);
    MDNS.addService(HOSTNAME, "tcp", 80);
    server.begin();
    show_ip();
  } else if (!connected && was_connected) {
    Serial.println("[net] wifi lost");
    ui_set_status("Wi-Fi reconnecting");
  }
  was_connected = connected;
  if (!connected) return;

  server.handleClient();

  if (last_usage_ms != 0 && !stale_shown && millis() - last_usage_ms > STALE_MS) {
    stale_shown = true;
    ui_set_status("PC offline");
  }
}
