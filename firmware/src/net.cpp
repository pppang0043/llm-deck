#include "net.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "link.h"
#include "mic.h"
#include "secrets.h"
#include "ui.h"

#define HOSTNAME "llm-deck"
#define AP_SSID "LLM-Deck"
#define AP_PASSWORD "llmdeck1234"

static WebServer server(80);
static bool station = false;
static bool was_connected = false;

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

  station = strlen(WIFI_SSID) > 0;
  if (station) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    ui_set_wifi_info("connecting...");
    return;
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  start_services();
  Serial.printf("[net] access point %s, ip %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  ui_set_wifi_info("join " AP_SSID " / " AP_PASSWORD);
}

void net_loop() {
  if (station) {
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !was_connected) {
      Serial.printf("[net] connected, ip %s\n", WiFi.localIP().toString().c_str());
      start_services();
      ui_set_wifi_info(WiFi.localIP().toString().c_str());
    } else if (!connected && was_connected) {
      Serial.println("[net] wifi lost");
      ui_set_wifi_info("reconnecting...");
    }
    was_connected = connected;
    if (!connected) return;
  }
  server.handleClient();
}
