#include "net.h"

#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <initializer_list>
#include <mbedtls/md.h>

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
    // 못 찾았을 때는 한동안 쉬었다가 다시 찾는다. 블루투스로 PC 와 연결되어 있으면 더 드물게 찾는다
    // (Wi-Fi 가 우선이라 돌아오면 바꿔 타야 하지만, Wi-Fi 를 막아 둔 곳에서는 블루투스가 자꾸 끊기면 안 된다)
    static uint32_t ble_tried_ms = 0;
    bool searched = false;
    bool on_ble = link_active() == LINK_BLE;
    if (radio_on && station && WiFi.status() != WL_CONNECTED &&
        (!on_ble || !ble_tried_ms || millis() - ble_tried_ms > 300000)) {
      if (on_ble) ble_tried_ms = millis();
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

// ---- Wi-Fi 인증 (net.h 의 설명) ----
static uint8_t key[32];
static bool have_key = false;
// /status 가 내준 nonce 들. 한 PC 의 도우미가 하나씩 쓴다 (순번은 받은 것 중 가장 큰 것과 그 아래 32개를 기억)
#define SESSIONS 6
struct Session {
  uint8_t nonce[16];
  uint32_t last;  // 받은 가장 큰 순번
  uint32_t seen;  // last 아래 32개 중 받은 것 (비트)
  uint32_t used_ms;
  bool live, used;
};
static Session sessions[SESSIONS];
// 기기 -> PC UDP 서명에 쓰는 nonce (마지막으로 인증된 /usage 를 보낸 PC 의 것)
static portMUX_TYPE tx_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t tx_nonce[16];
static bool tx_ready = false;
static uint32_t tx_ctr = 0;

struct Part {
  const void *p;
  size_t n;
};

static void sign(std::initializer_list<Part> parts, uint8_t out[32]) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, key, sizeof(key));
  for (const Part &part : parts) mbedtls_md_hmac_update(&ctx, (const uint8_t *)part.p, part.n);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

static void to_hex(const uint8_t *data, size_t len, char *out) {
  for (size_t i = 0; i < len; i++) sprintf(out + i * 2, "%02x", data[i]);
}

static bool from_hex(const char *text, size_t len, uint8_t *out) {
  for (size_t i = 0; i < len; i++) {
    unsigned v;
    if (!isxdigit((unsigned char)text[i * 2]) || !isxdigit((unsigned char)text[i * 2 + 1])) return false;
    sscanf(text + i * 2, "%2x", &v);
    out[i] = v;
  }
  return true;
}

// 기기 이름표: Wi-Fi MAC 12자. PC 가 여러 기기의 키를 구별한다
static String device_id() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char hex[13];
  to_hex(mac, 6, hex);
  return hex;
}

String net_pair_json() {
  if (!have_key) {
    // 무선이 켜진 뒤라 하드웨어 난수가 진짜 난수다
    esp_fill_random(key, sizeof(key));
    prefs.putBytes("wkey", key, sizeof(key));
    have_key = true;
    Serial.println("[net] new wifi key made");
  }
  char hex[65];
  to_hex(key, sizeof(key), hex);
  return String("{\"pair\":{\"id\":\"") + device_id() + "\",\"key\":\"" + hex + "\"}}";
}

// 새 nonce 를 내준다. 아직 안 쓴 것 중 가장 오래된 칸을 먼저 덮어써서, 아무나 /status 를 많이 불러도
// 쓰고 있는 PC 의 것은 (빈 칸이 없을 때 한 번 말고는) 지워지지 않는다
static void new_session(char hex[33]) {
  int pick = -1;
  for (int i = 0; i < SESSIONS && pick < 0; i++) {
    if (!sessions[i].live) pick = i;
  }
  for (int pass = 0; pass < 2 && pick < 0; pass++) {
    uint32_t oldest = 0;
    for (int i = 0; i < SESSIONS; i++) {
      if (pass == 0 && sessions[i].used) continue;
      uint32_t age = millis() - sessions[i].used_ms;
      if (pick < 0 || age > oldest) pick = i, oldest = age;
    }
  }
  Session &s = sessions[pick];
  esp_fill_random(s.nonce, sizeof(s.nonce));
  s.last = 0;
  s.seen = 1;  // 순번 0 은 받지 않는다
  s.used_ms = millis();
  s.live = true;
  s.used = false;
  to_hex(s.nonce, sizeof(s.nonce), hex);
}

enum Auth { AUTH_OK, AUTH_UNPAIRED, AUTH_STALE, AUTH_BAD };
static const char *auth_name(Auth a) {
  switch (a) {
    case AUTH_UNPAIRED: return "unpaired";
    case AUTH_STALE: return "stale";
    case AUTH_BAD: return "bad";
    default: return "ok";
  }
}

// X-Deck-Auth 머리글 확인. 맞으면 *slot 에 그 PC 의 칸
static Auth check_auth(const char *path, const String &body, int *slot) {
  if (!have_key) return AUTH_UNPAIRED;
  String head = server.header("X-Deck-Auth");
  // <nonce 32자>.<순번>.<서명 64자>
  int dot2 = head.lastIndexOf('.');
  if (head.length() < 32 + 1 + 1 + 1 + 64 || head[32] != '.' || dot2 != (int)head.length() - 65) return AUTH_BAD;
  uint8_t nonce[16], tag[32], want[32];
  if (!from_hex(head.c_str(), 16, nonce) || !from_hex(head.c_str() + dot2 + 1, 32, tag)) return AUTH_BAD;
  char *end;
  unsigned long ctr = strtoul(head.c_str() + 33, &end, 10);
  if (end != head.c_str() + dot2 || ctr == 0 || ctr > 0xFFFFFFFFUL) return AUTH_BAD;
  int i = 0;
  while (i < SESSIONS && !(sessions[i].live && memcmp(sessions[i].nonce, nonce, 16) == 0)) i++;
  if (i == SESSIONS) return AUTH_STALE;  // 기기가 다시 켜졌거나 오래된 nonce
  uint32_t c = ctr;
  sign({{"http", 4}, {nonce, 16}, {&c, 4}, {path, strlen(path)}, {"\n", 1}, {body.c_str(), body.length()}}, want);
  uint8_t diff = 0;
  for (int k = 0; k < 32; k++) diff |= tag[k] ^ want[k];
  if (diff) return AUTH_BAD;
  // 같은 순번은 한 번만 (요청 순서가 조금 뒤바뀌는 것은 받는다)
  Session &s = sessions[i];
  if (c > s.last) {
    uint32_t shift = c - s.last;
    s.seen = shift >= 32 ? 1 : (s.seen << shift) | 1;
    s.last = c;
  } else {
    uint32_t back = s.last - c;
    if (back >= 32 || (s.seen >> back) & 1) return AUTH_STALE;
    s.seen |= 1u << back;
  }
  s.used = true;
  s.used_ms = millis();
  *slot = i;
  return AUTH_OK;
}

// 인증이 안 되면 401 로 답한다. 키가 없거나 틀린 PC 면 기기 화면에도 알린다 (가끔만)
static bool authorized(const char *path, const String &body, int *slot) {
  Auth a = check_auth(path, body, slot);
  if (a == AUTH_OK) return true;
  server.send(401, "application/json", String("{\"auth\":\"") + auth_name(a) + "\"}");
  if (a == AUTH_STALE) return false;  // 기기가 다시 켜진 뒤 등: PC 가 새 nonce 를 받아 다시 보낸다
  static uint32_t warned_ms = 0;
  if (!warned_ms || millis() - warned_ms > 600000) {
    warned_ms = millis();
    Serial.printf("[net] refused %s from %s (%s): connect the PC by USB once to use Wi-Fi\n", path,
                  server.client().remoteIP().toString().c_str(), auth_name(a));
    ui_toast("Wi-Fi 연결을 거절했습니다\nUSB로 한 번 연결하면\nWi-Fi를 쓸 수 있습니다", 6000);
  }
  return false;
}

bool net_udp_trailer(const uint8_t *data, size_t len, uint8_t *trailer) {
  uint8_t nonce[16];
  uint32_t ctr = 0;
  portENTER_CRITICAL(&tx_mux);
  bool ready = tx_ready;
  if (ready) {
    memcpy(nonce, tx_nonce, 16);
    ctr = ++tx_ctr;
  }
  portEXIT_CRITICAL(&tx_mux);
  if (!ready) return false;
  uint8_t tag[32];
  sign({{"udp", 3}, {nonce, 16}, {&ctr, 4}, {data, len}}, tag);
  memcpy(trailer, &ctr, 4);
  memcpy(trailer + 4, tag, 16);
  return true;
}

bool net_tcp_hello(WiFiClient &client) {
  if (!have_key) return false;
  uint8_t challenge[16];
  size_t got = 0;
  uint32_t start = millis();
  while (got < sizeof(challenge) && millis() - start < 3000 && client.connected()) {
    int n = client.read(challenge + got, sizeof(challenge) - got);
    if (n > 0) {
      got += n;
    } else {
      delay(5);
    }
  }
  if (got < sizeof(challenge)) return false;
  uint8_t frame[5 + 32] = {0xA5, 0x5A, NET_FRAME_AUTH, 32, 0};
  sign({{"tcp", 3}, {challenge, sizeof(challenge)}}, frame + 5);
  return client.write(frame, sizeof(frame)) == sizeof(frame);
}

static void send_state() {
  JsonDocument doc(PSRAM_JSON);
  doc["ok"] = true;
  doc["mic"] = mic_is_on();
  doc["link"] = settings_link_allowed(LINK_WIFI);  // 꺼져 있으면 방금 보낸 것은 버렸다
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// 기기 찾기용. 누구나 부를 수 있어서 상태는 조금만 알려 주고, 인증에 쓸 새 nonce 를 준다
static void send_status() {
  JsonDocument doc(PSRAM_JSON);
  char nonce[33];
  new_session(nonce);
  doc["ok"] = true;
  doc["mic"] = mic_is_on();
  doc["id"] = device_id();
  doc["auth"] = 1;
  doc["proto"] = FW_PROTO;  // 도우미와 주고받는 내용의 버전 (link.h)
  // 설정에서 Wi-Fi 연결을 껐으면(클라우드만 켜 둔 경우) 보내도 버리므로, 도우미가 다른 방식(블루투스)으로 넘어가게 알린다
  doc["link"] = settings_link_allowed(LINK_WIFI);
  doc["nonce"] = nonce;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handle_usage() {
  const String &body = server.arg("plain");
  int slot;
  if (!authorized("/usage", body, &slot)) return;
  // 인증된 PC 로 마이크 소리·알림을 보낸다 (아무나 소리를 자기 쪽으로 돌리지 못한다)
  mic_set_target(server.client().remoteIP());
  portENTER_CRITICAL(&tx_mux);
  memcpy(tx_nonce, sessions[slot].nonce, 16);
  tx_ready = true;
  portEXIT_CRITICAL(&tx_mux);
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
  server.on("/status", HTTP_GET, send_status);
  static const char *headers[] = {"X-Deck-Auth"};
  server.collectHeaders(headers, 1);
  // 마지막으로 찍은 사진. 사진을 찍으면 PC 에 알림을 보내고, PC 가 여기서 받아 간다.
  server.on("/photo", HTTP_GET, [] {
    int slot;
    if (!authorized("/photo", String(), &slot)) return;
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
  have_key = prefs.getBytes("wkey", key, sizeof(key)) == sizeof(key);
  if (!have_key) Serial.println("[net] no wifi key yet: connect the PC by USB once to use Wi-Fi");
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
