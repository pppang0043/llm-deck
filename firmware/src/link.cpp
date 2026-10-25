#include "link.h"
#include "build_identity.h"
#include <esp_app_desc.h>

#include <ArduinoJson.h>
#include <Preferences.h>
#include <sys/time.h>
#include <string>

#include <SD.h>
#include <WiFiUdp.h>
#include <WiFi.h>

#include "psram_json.h"
#include "message_queue.h"
#include "command_journal.h"
#include "command_protocol.h"
#include "control_policy.h"
#include <mbedtls/sha256.h>
#include <memory>
#include "alerts.h"
#include "askpanel.h"
#include "orcapanel.h"
#include "settings.h"
#include "battery.h"
#include "ble_link.h"
#include "cloud.h"
#include <img_converters.h>
#include <lvgl.h>

#include "camera.h"
#include "display.h"
#include "health.h"
#include "mirror.h"
#include "pcpanel.h"
#include "camera_ui.h"
#include "mic.h"
#include "photo.h"
#include "saver.h"
#include "sd_video.h"
#include "storage.h"
#include "pcmic.h"
#include "usb_link.h"
#include "net.h"
#include "ui.h"

static const char *firmware_build_id() {
  static char id[21] = {};
  if (!id[0]) {
    const uint8_t *sha = esp_app_get_description()->app_elf_sha256;
    for (int i = 0; i < 10; ++i) snprintf(id + i * 2, 3, "%02x", sha[i]);
  }
  return id;
}

// 받은 사용량을 플래시에 저장하는 최소 간격. 플래시는 쓰기 횟수에 수명이 있어 자주 쓰지 않는다.
#define SAVE_INTERVAL_MS 300000
// 플래시(NVS)에 저장할 수 있는 글자 수 한계
#define SAVE_MAX_BYTES 3900

// 메시지는 PSRAM 에만 둔다. 부족하면 기존 명령을 보존하고 새 요청을 거절한다.
static void *message_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static SemaphoreHandle_t lock;
static MessageQueue pending(message_alloc, heap_caps_free);
static Preferences command_prefs;
static SemaphoreHandle_t command_lock;
static bool save_commands(const CommandJournal::Storage &data) {
  return command_prefs.putBytes("journal", &data, sizeof(data)) == sizeof(data);
}
static CommandJournal commands(save_commands);

// 생존 신호는 큐를 차지하지 않는다. loop 에서만 연결/UI 상태를 바꾼다.
static uint32_t heartbeat_ms[5] = {};
static uint32_t lease_ms[5] = {};

static volatile Link active = LINK_NONE;
static HelperInfo helper = {};
// 이번 연결에서 펌웨어 버전을 알렸는지, 버전이 달라 알림을 띄웠는지
static bool told_proto = false;
static bool warned_proto = false;
static void (*set_mic_cb)(bool);
static void (*rotate_cb)() = nullptr;

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

// 여러 PC 가 동시에 보내면 "현재 연결"이 30초마다 바뀌어 마이크 소리·사진이 오락가락한다.
// 그래서 우선순위를 둔다: USB > Wi-Fi > 블루투스 > 클라우드. 높은 쪽이 살아 있으면 낮은 쪽은
// 현재 연결을 가져가지 못한다. 표시와 제어 명령은 선택된 PC 만 사용한다.
#define LINK_FRESH_MS 75000
static uint32_t heard_ms[5] = {0};

static int priority(Link link) {
  switch (link) {
    case LINK_USB: return 4;
    case LINK_WIFI: return 3;  // 블루투스보다 빠르고 인증도 된다. 블루투스는 Wi-Fi 를 막아 둔 곳(회사 WIPS 등)에서 쓴다
    case LINK_BLE: return 2;
    case LINK_CLOUD: return 1;
    default: return 0;
  }
}

static bool alive(Link link) {
  if (!settings_link_allowed(link)) return false;
  if (!heard_ms[link] || millis() - heard_ms[link] > (lease_ms[link] ? lease_ms[link] : LINK_FRESH_MS)) return false;
  if (link == LINK_WIFI && WiFi.status() != WL_CONNECTED) return false;
  // 케이블을 뽑거나 블루투스가 끊기면 기다리지 않고 바로 내려놓는다
  if (link == LINK_USB && !usb_host_present()) return false;
  if (link == LINK_BLE && !ble_link_connected()) return false;
  return true;
}

static Link best_link() {
  Link best = LINK_NONE;
  for (Link l : {LINK_USB, LINK_WIFI, LINK_BLE, LINK_CLOUD}) {
    if (alive(l) && priority(l) > priority(best)) best = l;
  }
  return best;
}

static void set_active(Link link) {
  if (link == active) return;
  active = link;
  told_proto = warned_proto = false;
  ui_set_status(link_name(link));
  Serial.printf("[link] active: %s\n", link_tag(link));
}

// 사용량과 함께 온 도우미 상태를 기억하고, 버전을 서로 맞춰 본다 (연결마다 한 번)
static void note_helper(JsonDocument &doc) {
  JsonObjectConst h = doc["helper"];
  // 같은 연결 방식이라도 다른 PC 로 바뀌면 다시 맞춰 본다
  if (strcmp(helper.host, doc["host"] | "") != 0) told_proto = warned_proto = false;
  helper.known = true;
  helper.proto = h["proto"] | 0;
  strlcpy(helper.host, doc["host"] | "", sizeof(helper.host));
  strlcpy(helper.ver, h["ver"] | "", sizeof(helper.ver));
  helper.at = h["at"] | (int64_t)0;
  strlcpy(helper.err, h["err"] | "", sizeof(helper.err));
  // 도우미가 다시 켜져 버전을 모르면 다시 묻는다
  if (doc["ask_fw"] == true) told_proto = false;
  if (!told_proto) {
    told_proto = true;
    char json[128];
    snprintf(json, sizeof(json), "{\"fw_proto\":%d,\"fw\":\"%s\",\"build\":\"%s\"}", FW_PROTO, __DATE__ " " __TIME__, firmware_build_id());
    link_send_event(json);
  }
  if (helper.proto != FW_PROTO && !warned_proto) {
    warned_proto = true;
    Serial.printf("[link] helper proto %d, firmware proto %d\n", helper.proto, FW_PROTO);
    ui_toast(helper.proto < FW_PROTO ? "PC 도우미가 펌웨어보다 오래되었습니다. PC 도우미를 업데이트하세요"
                                     : "펌웨어가 PC 도우미보다 오래되었습니다. 펌웨어를 올리세요",
             8000);
  }
}

void link_init(void (*set_mic)(bool on)) {
  (void)firmware_build_id();
  set_mic_cb = set_mic;
  lock = xSemaphoreCreateMutex();
  command_lock = xSemaphoreCreateMutex();
  CommandJournal::Storage journal;
  if (!command_prefs.begin("cmd-journal")) {
    commands.load(nullptr, 1); // 저장소가 없으면 보호 명령은 거절한다
  } else if (!command_prefs.isKey("journal")) {
    commands.load(nullptr, 0);
  } else if (command_prefs.getBytesLength("journal") == sizeof(journal) &&
             command_prefs.getBytes("journal", &journal, sizeof(journal)) == sizeof(journal)) {
    commands.load(&journal, sizeof(journal));
  } else {
    commands.load(nullptr, 1);
  }
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

// 상태만 들어 있는 메시지는 같은 필드 집합끼리 최신 것으로 합친다.
// 명령이 하나라도 섞였으면 전체를 FIFO 에 보존한다 (촬영 직전 화면 갱신 등).
static uint32_t snapshot_key(JsonObjectConst doc) {
  static const char *const fields[] = {"services", "updated", "host", "now", "utc_offset", "helper",
                                      "ask_mic", "keywords", "os", "pc", "orca", "sync_url", "pc_mic"};
  uint32_t mask = 0;
  for (JsonPairConst item : doc) {
    bool known = false;
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
      if (strcmp(item.key().c_str(), fields[i]) == 0) { mask |= 1U << i; known = true; break; }
    }
    if (!known) return 0;
  }
  return mask;
}

static void command_reply(String *reply, const char *client, uint32_t seq, const char *state) {
  if (!reply) return;
  char text[128];
  snprintf(text, sizeof(text), "{\"ack\":[\"%s\",%lu,\"%s\"]}", client, (unsigned long)seq, state);
  *reply = text;
}

bool link_submit(Link from, const char *data, size_t len, bool usage, String *reply, uint64_t peer) {
  if (from <= LINK_NONE || from > LINK_CLOUD || !len || len > MessageQueue::MAX_MESSAGE) return false;
  JsonDocument doc(PSRAM_JSON);
  if (deserializeJson(doc, data, len) || !doc.is<JsonObjectConst>()) return false;
  // 업로더의 무작위 도전값을 그대로 돌려준다. 오래된 로그를 새 부팅 확인으로 착각하지 않는다.
  if (doc["firmware_info"].is<const char *>() && doc.size() == 1 && from != LINK_CLOUD) {
    const char *probe = doc["firmware_info"];
    if (strlen(probe) != 32) return false;
    for (int i = 0; i < 32; ++i) if (!isxdigit((unsigned char)probe[i])) return false;
    if (reply) *reply = String("{\"fw_proto\":") + FW_PROTO + ",\"build\":\"" + firmware_build_id() +
                        "\",\"id\":\"" + net_device_id() + "\",\"probe\":\"" + probe + "\"}";
    return true;
  }
  // 명령/조회는 보낸 통신 경로로 직접 답한다. 활성 PC 로 보내면 다른 PC 가 ACK 를 받게 된다.
  if (doc["command_protocol"] == true && doc.size() == 1 && from != LINK_CLOUD) {
    if (reply) *reply = String("{\"command_proto\":1,\"id\":\"") + net_device_id() + "\"}";
    return true;
  }
  bool query = doc["command_status"].is<JsonObjectConst>();
  bool control = doc["command"].is<JsonObjectConst>();
  if (query || control) {
    if (doc.size() != 1 || from == LINK_CLOUD) return false;
    JsonObjectConst request = doc[query ? "command_status" : "command"];
    const char *client = request["client"] | "";
    if (!CommandJournal::valid_client(client) || !request["seq"].is<uint32_t>() || request["seq"].as<uint32_t>() == 0) return false;
    uint32_t seq = request["seq"];
    if (query) {
      xSemaphoreTake(command_lock, portMAX_DELAY);
      command_reply(reply, client, seq, commands.status(client, seq));
      xSemaphoreGive(command_lock);
      return true;
    }
    const char *payload = request["json"] | "";
    JsonDocument body(PSRAM_JSON);
    if (!payload[0] || deserializeJson(body, payload) || !valid_command(body.as<JsonObjectConst>())) {
      command_reply(reply, client, seq, "invalid");
      return true;
    }
    if (from != LINK_USB) {
      for (const char *key : {"links", "sd_pull", "sd_clear", "health_clear", "hang_test", "crash_test", "wifi_add", "wifi_clear"}) {
        if (!body[key].isNull()) { command_reply(reply, client, seq, "forbidden"); return true; }
      }
    }
    if (!control_allowed(from, active, usage, body.as<JsonObjectConst>())) {
      command_reply(reply, client, seq, "forbidden");
      return true;
    }
    CommandJournal::Request req;
    strlcpy(req.client, client, sizeof(req.client));
    req.seq = seq;
    if (mbedtls_sha256((const unsigned char *)payload, strlen(payload), req.digest, 0) != 0) return false;
    xSemaphoreTake(command_lock, portMAX_DELAY);
    int ticket;
    const char *state = commands.reserve(req, ticket);
    if (strcmp(state, "new") == 0) {
      xSemaphoreTake(lock, portMAX_DELAY);
      bool ok = pending.push(from, usage, 0, data, len, ticket, peer);
      xSemaphoreGive(lock);
      if (!ok) commands.cancel(ticket);
      state = ok ? "queued" : "rejected";
    }
    command_reply(reply, client, seq, state);
    xSemaphoreGive(command_lock);
    return true;
  }
  // 순수 생존 신호만 바로 받는다. 제어 명령이 함께 있으면 생략하지 않는다.
  if (doc["heartbeat"] == true && doc.size() == 1 && from != LINK_CLOUD) {
    if (!usage) return true;  // 대기 BLE PC 는 사용 중인 PC 의 생존 시간을 늘리지 않는다
    xSemaphoreTake(lock, portMAX_DELAY);
    heartbeat_ms[from] = millis() ? millis() : 1;
    xSemaphoreGive(lock);
    return true;
  }
  if (has_control(doc.as<JsonObjectConst>()) && !control_allowed(from, active, usage, doc.as<JsonObjectConst>())) {
    if (reply) *reply = "{\"link_error\":\"not_owner\"}";
    return true;
  }
  if (!usage && !has_control(doc.as<JsonObjectConst>())) return true; // 대기 PC 의 상태는 큐도 차지하지 않는다
  uint32_t snapshot = usage ? snapshot_key(doc.as<JsonObjectConst>()) : 0;
  xSemaphoreTake(lock, portMAX_DELAY);
  bool ok = pending.push(from, usage, snapshot, data, len, -1, peer);
  xSemaphoreGive(lock);
  if (!ok) Serial.printf("[link] rejected message from %s: queue/memory limit\n", link_tag(from));
  return ok;
}

void link_loop() {
  MessageQueue::Item item;
  xSemaphoreTake(lock, portMAX_DELAY);
  pending.pop(item);
  for (int i = LINK_USB; i <= LINK_WIFI; ++i) {
    if (heartbeat_ms[i]) {
      heard_ms[i] = heartbeat_ms[i];
      lease_ms[i] = 15000;
      heartbeat_ms[i] = 0;
    }
  }
  xSemaphoreGive(lock);
  // 큐가 비어 있어도 생존 신호로 활성 연결을 고른다.
  set_active(best_link());
  std::unique_ptr<char, decltype(&heap_caps_free)> owned(item.data, heap_caps_free);
  const char *message = item.data;
  Link from = static_cast<Link>(item.from);
  bool usage = item.usage;
  // 보호 명령은 실행하기 전에 NVS 에 ID/내용 해시를 남긴다. 재부팅 후에도 같은 ID 는 재실행하지 않는다.
  JsonDocument envelope(PSRAM_JSON);
  CommandJournal::Request executed;
  bool durable = false, command_ok = true, links_handled = false;
  if (item.command_ticket >= 0) {
    if (deserializeJson(envelope, message, item.len)) {
      xSemaphoreTake(command_lock, portMAX_DELAY);
      commands.cancel(item.command_ticket);
      xSemaphoreGive(command_lock);
      message = nullptr;
    } else {
      xSemaphoreTake(command_lock, portMAX_DELAY);
      durable = commands.start(item.command_ticket, executed);
      xSemaphoreGive(command_lock);
      message = durable ? envelope["command"]["json"].as<const char *>() : nullptr;
      if (message) item.len = strlen(message);
    }
  }


  // 수신 당시 허용됐어도 큐에 있는 동안 다른 PC 로 전환됐을 수 있다.
  if (message) {
    JsonDocument policy(PSRAM_JSON);
    bool selected = usage;
    if (from == LINK_BLE) selected = item.peer && item.peer == ble_primary_cookie();
    if (from == LINK_WIFI) selected = net_peer_is_owner((uint32_t)item.peer);
    usage = selected;
    if (deserializeJson(policy, message, item.len) || (!selected && !has_control(policy.as<JsonObjectConst>())) ||
        (has_control(policy.as<JsonObjectConst>()) && !control_allowed(from, active, selected, policy.as<JsonObjectConst>()))) {
      command_ok = false;
      message = nullptr;
    }
  }

  // USB 로 온 {"links": {"usb": true, "ble": false, ...}} 는 연결 방식 설정을 바꾼다. USB 를 꺼 둔 상태에서도
  // 받는다 (케이블을 직접 꽂은 PC 이므로 믿고, USB 를 실수로 꺼도 PC 에서 되살릴 수 있게).
  if (message != nullptr && from == LINK_USB && strstr(message, "\"links\"") != nullptr) {
    JsonDocument doc(PSRAM_JSON);
    if (!deserializeJson(doc, message, item.len) && doc["links"].is<JsonObjectConst>()) {
      links_handled = true;
      JsonObjectConst links = doc["links"];
      if (links["usb"].is<bool>()) settings_set_link_allowed(LINK_USB, links["usb"]);
      if (links["ble"].is<bool>()) settings_set_link_allowed(LINK_BLE, links["ble"]);
      if (links["wifi"].is<bool>()) settings_set_link_allowed(LINK_WIFI, links["wifi"]);
      if (links["cloud"].is<bool>()) settings_set_link_allowed(LINK_CLOUD, links["cloud"]);
    }
  }
  // USB 로 온 {"pair": true} 에는 Wi-Fi 인증 키로 답한다 (net.h). 케이블을 직접 꽂은 PC 만 믿고, 블루투스는
  // 암호화 없이 연결되어 엿들을 수 있으므로 주지 않는다. 키가 로그에 남지 않게 알림 기록 없이 바로 보낸다
  if (message != nullptr && from == LINK_USB && strstr(message, "\"pair\"") != nullptr) {
    JsonDocument doc(PSRAM_JSON);
    if (!deserializeJson(doc, message, item.len) && doc["pair"] == true) {
      usb_send_event(net_pair_json().c_str());
      Serial.println("[link] wifi key given to the USB PC");
    }
  }
  // 설정에서 끈 연결 방식으로 온 것은 버린다
  if (message != nullptr && !settings_link_allowed(from)) {
    static uint32_t warned_ms = 0;
    if (millis() - warned_ms > 60000) {
      warned_ms = millis();
      Serial.printf("[link] ignoring %s (turned off in settings)\n", link_tag(from));
    }
    if (!links_handled) command_ok = false;
    message = nullptr;
  }
  if (message != nullptr) {
    JsonDocument doc(PSRAM_JSON);
    if (deserializeJson(doc, message, item.len)) {
      command_ok = false;
      Serial.printf("[link] bad json from %s (%u bytes)\n", link_tag(from), (unsigned)item.len);
    } else {
      if (usage) heard_ms[from] = millis();
      // 이전 도우미는 30초마다 보내므로 기존 75초 유예를 유지한다.
      if (doc["helper"]["proto"].is<int>() && doc["helper"]["proto"].as<int>() < 4) lease_ms[from] = 0;
      Link best = best_link();
      set_active(best == LINK_NONE ? from : best);
      if (from == active && usage && doc["now"].is<int64_t>()) set_clock(doc["now"], doc["utc_offset"] | 32400);
      if (doc["mic"].is<bool>()) set_mic_cb(doc["mic"]);
      if (doc["battery_mv"].is<int>()) battery_calibrate(doc["battery_mv"]);
      // PC 에서 화면을 바꾸면 사람이 만진 것처럼 화면 보호기·절전을 깨운다
      if (doc["camera"].is<const char *>()) {
        display_poke();
        if (!camera_ui_command(doc["camera"])) command_ok = false;
      }
      if (doc["view"].is<const char *>()) {
        display_poke();
        ui_show(doc["view"]);
      }
      if (doc["camera_mount"].is<JsonObjectConst>()) {
        camera_set_mount(doc["camera_mount"]["rot"] | 0, doc["camera_mount"]["mirror"] | false);
      }
      if (doc["camera_ctl"].is<JsonObjectConst>()) {
        for (JsonPairConst c : doc["camera_ctl"].as<JsonObjectConst>()) {
          if (!camera_ctl_set(c.key().c_str(), c.value() | 0)) command_ok = false;
        }
      }
      if (doc["saver"] == true) saver_demo();
      // 진단용: I2C 데이터 선을 잠깐 낮게 잡아 막힌 상황을 만든다 (막힘 풀기가 동작하는지 본다)
      if (doc["i2c_test"] == true) display_i2c_test();
      // 진단용: PC 를 다시 켜 USB 가 꼬였을 때 하는 USB 다시 붙이기를 해 본다
      if (doc["usb_reconnect"] == true) usb_reconnect("asked by the PC");
      // 진단용: 화면 돌리기 버튼을 누른 것과 같다
      if (doc["rotate"] == true && rotate_cb) rotate_cb();
      if (doc["photo_test"] == true) {
        uint8_t *jpg;
        size_t len;
        if (camera_test_pattern(&jpg, &len)) photo_save(jpg, len);
      }
      if (from == active && usage && doc["sync_url"].is<const char *>()) cloud_set_url(doc["sync_url"]);
      if (doc["wifi_add"]["ssid"].is<const char *>()) net_add_wifi(doc["wifi_add"]["ssid"], doc["wifi_add"]["password"] | "");
      if (doc["wifi_clear"] | false) net_clear_wifi();
      if (doc["services"].is<JsonArrayConst>() && !usage) {
        static uint32_t told_ms = 0;
        if (!told_ms || millis() - told_ms > 60000) {
          told_ms = millis();
          Serial.printf("[link] usage from standby Bluetooth PC %s ignored\n", doc["host"] | "?");
        }
      }
      if (doc["ble_use"].is<int>()) ble_peer_select(doc["ble_use"]);
      if (doc["bt_check"] == true) ble_speaker_check();
      if (doc["bt_check"].is<const char *>()) ble_speaker_check(doc["bt_check"]);
      // 진단용: SD 카드의 마지막 동영상을 PC 로 보낸다 (케이블로 연결한 PC 에서만)
      if (doc["sd_pull"] == true && from == LINK_USB) {
        // 메모리에 다 올리지 않고 카드에서 조금씩 읽어 보낸다
        String last;
        File dir = storage_ready() ? SD.open("/videos") : File();
        for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
          if (!f.isDirectory() && String(f.name()) > last) last = f.name();
          f.close();
        }
        if (dir) dir.close();
        if (!last.length() || !photo_send_sd_file(("/videos/" + last).c_str())) command_ok = false;
      }
      // 진단용: SD 카드의 사진을 모두 지운다 (케이블로 연결한 PC 에서만)
      // 진단용(USB 만): 일부러 프로그램을 멈춰 재시작 원인 기록이 되는지 본다
      if (doc["health_clear"] == true && from == LINK_USB) health_clear();
      if (doc["hang_test"] == true && from == LINK_USB) {
        // 진단용(USB 만): 일부러 멈춰서 감시 장치가 다시 켜는지 본다
        Serial.println("[health] hang test");
        for (;;) delay(1000);
      }
      if (doc["crash_test"] == true && from == LINK_USB) {
        Serial.println("[health] crash test");
        Serial.flush();
        delay(100);
        abort();
      }
      if (doc["sd_clear"] == true && from == LINK_USB && sd_video_active()) {
        command_ok = false;
        Serial.println("[link] sd_clear refused: video still being written");
      } else if (doc["sd_clear"] == true && from == LINK_USB) {
        storage_clear("/photos");
        storage_clear("/videos");
      }
      // 사용 중인 PC 가 보낸 PC 상태와 알림 글 (단축 버튼을 눌렀을 때 결과 등)
      if (doc["pc"].is<JsonObjectConst>() && from == active && usage) pcpanel_set_status(doc["pc"], doc["host"] | "");
      if (doc["os"].is<const char *>() && from == active && usage) pcpanel_set_os(doc["os"]);
      if (doc["keywords"].is<JsonArrayConst>() && from == active && usage) pcpanel_set_keywords(doc["keywords"]);
      if (doc["toast"].is<const char *>() && from == active && usage) ui_toast(doc["toast"], 3000);
      if (doc["scan"].is<const char *>() && from == active && usage) camera_ui_scan_result(doc["scan"]);
      if (doc["scan_roi"].is<JsonArrayConst>() && from == active && usage) {
        JsonArrayConst a = doc["scan_roi"];
        int box[4] = {a[0] | 0, a[1] | 0, a[2] | 0, a[3] | 0};
        camera_ui_scan_roi(box, a.size() == 4);
      }
      if (doc["ask"].is<JsonObjectConst>() && from == active && usage) askpanel_set(doc["ask"]);
      if (doc["ask_mic"].is<bool>() && from == active && usage) askpanel_set_mic(doc["ask_mic"]);
      if (doc["pc_mic"].is<JsonObjectConst>() && from == active && usage) pcmic_set(doc["pc_mic"]);
      if (doc["orca"].is<JsonObjectConst>() && from == active && usage) orcapanel_set(doc["orca"]);
      if (doc["services"].is<JsonArrayConst>() && from == active && usage) {
        JsonArrayConst services = doc["services"];
        ui_set_usage(services);
        alerts_check(services);
        shown.clear();
        serializeJson(services, shown);
        Serial.printf("[link] usage via %s: %u services (%s)\n", link_tag(from), (unsigned)services.size(),
                      doc["host"] | "?");
        note_helper(doc);
      }
      // 미러링 창 (USB, Wi-Fi)
      bool mirror_link = from == LINK_USB || from == LINK_WIFI;
      if (doc["mirror"].is<bool>() && mirror_link) mirror_keep(doc["mirror"]);
      if (doc["touch"].is<JsonObjectConst>() && mirror_link) {
        display_remote_touch(doc["touch"]["x"] | 0, doc["touch"]["y"] | 0, doc["touch"]["down"] | false);
      }
      for (JsonObjectConst t : doc["touch"].as<JsonArrayConst>()) {
        if (mirror_link) display_remote_touch(t["x"] | 0, t["y"] | 0, t["down"] | false);
      }
      if (doc["screenshot"] == true) {
        // 진단용: 지금 화면을 사진처럼 PC 로 보낸다 (같이 온 목록, 알림까지 그린 뒤)
        uint16_t *px;
        int w, h;
        if (ui_settings_visible()) settings_redraw();
        lv_refr_now(NULL);
        if (ui_screenshot(&px, &w, &h)) {
          for (int i = 0; i < w * h; i++) px[i] = (px[i] << 8) | (px[i] >> 8);
          uint8_t *jpg;
          size_t len;
          if (fmt2jpg((uint8_t *)px, w * h * 2, w, h, PIXFORMAT_RGB565, 90, &jpg, &len)) photo_save(jpg, len, false);
          free(px);
        }
      }
    }
  }

  if (durable) {
    xSemaphoreTake(command_lock, portMAX_DELAY);
    commands.finish(executed, command_ok);
    xSemaphoreGive(command_lock);
  }
  save_if_due();
  if (active != LINK_NONE && !alive(active)) {
    // 지금 연결이 끊겼거나 설정에서 껐으면 살아 있는 다음 연결로 넘어간다 (없으면 "PC 없음")
    set_active(best_link());
  }
}

const HelperInfo &link_helper() {
  return helper;
}

void link_set_rotate(void (*rotate)()) {
  rotate_cb = rotate;
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
      // 서명을 붙인다 (net.h). 인증된 PC 가 없으면 보내지 않는다
      uint8_t trailer[NET_UDP_TRAILER];
      if ((uint32_t)mic_target() != 0 && net_udp_trailer((const uint8_t *)json, strlen(json), trailer)) {
        udp.beginPacket(mic_target(), MIC_UDP_PORT);
        udp.write((const uint8_t *)json, strlen(json));
        udp.write(trailer, sizeof(trailer));
        udp.endPacket();
      }
      break;
    default:
      return;
  }
  Serial.printf("[link] event to %s: %s\n", link_tag(active), json);
}
