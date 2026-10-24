#include "ble_link.h"

#include <NimBLEDevice.h>
#include <esp_bt.h>
#include <esp_chip_info.h>
#include <nimble/porting/nimble/include/os/os_mbuf.h>
#include <Preferences.h>
#include <esp_random.h>
#include <string>

#include "link.h"
#include "net.h"
#include "settings.h"
#include "ui.h"

#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // PC -> 기기
#define NUS_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // 기기 -> PC

#define MAX_MESSAGE 16384
#define AUDIO_HEADER 5
// 인증 (ble_link.h)
#define TAG_LEN 16
#define AUTH_TIMEOUT_MS 20000
#define HELLO "{\"ble_hello\":\""

static NimBLEServer *server;
static NimBLECharacteristic *tx;
// 여러 PC 가 동시에 붙을 수 있다 (최대 BLE_MAX_PEERS). 그중 하나만 "사용 중" 이고, 마이크 소리·사진·알림은
// 그 PC 로만 보내며 화면의 사용량도 그 PC 것을 쓴다. 나머지는 선택될 때까지 제어 명령도 기다린다.
// 사용 중인 PC 가 끊기면 기다리던 PC 가 이어받는다. 설정에서 고른 PC 는 기억해 두었다가 다시 붙으면 우선한다.
// 받는 중인 메시지는 PSRAM 에 둔다. 글자를 붙여 가며 커지는 버퍼를 내부 RAM 에 두면
// 내부 RAM 이 잘게 쪼개져 HTTPS(클라우드)·카메라가 큰 덩어리를 못 얻는다.
template <typename T>
struct PsramAlloc {
  using value_type = T;
  PsramAlloc() = default;
  template <typename U>
  PsramAlloc(const PsramAlloc<U> &) {}
  T *allocate(size_t n) {
    void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
    if (!p) abort();
    return (T *)p;
  }
  void deallocate(T *p, size_t) { heap_caps_free(p); }
  bool operator==(const PsramAlloc &) const { return true; }
  bool operator!=(const PsramAlloc &) const { return false; }
};
using PsramString = std::basic_string<char, std::char_traits<char>, PsramAlloc<char>>;

struct Peer {
  bool used;
  uint16_t handle;
  uint16_t mtu;
  char addr[20];
  char host[32];
  PsramString message;
  // 인증 (ble_link.h)
  bool hello;       // 도전값을 받아 기기 nonce 를 줬는지
  bool authed;      // 서명이 맞는 줄을 한 번이라도 보냈는지
  uint8_t pc_nonce[16];
  uint8_t dev_nonce[16];
  uint32_t rx_last;  // 받은 가장 큰 순번
  uint32_t tx_ctr;   // 보낸 알림 순번
  uint32_t since_ms;
};
static Peer peers[BLE_MAX_PEERS];
static portMUX_TYPE peers_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int primary = -1;
static volatile bool connected = false;  // 사용 중인 PC 가 있는지
static volatile bool enabled = true;
static volatile uint16_t mtu = 23;      // 사용 중인 PC 와의 MTU
static volatile uint16_t primary_handle = BLE_HS_CONN_HANDLE_NONE;
static Preferences prefs;
static char preferred[20] = "";
// 서명이 없거나 틀린 줄을 받았다 (메인 루프에서 알림을 띄운다. 블루투스 태스크에서는 화면을 만지지 않는다)
static volatile bool refused = false;

// IMA ADPCM: 16비트 샘플을 "이전 값과의 차이" 4비트로 줄이는 표준 방식
static const int8_t INDEX_TABLE[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static const int16_t STEP_TABLE[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static int32_t predictor = 0;
static int32_t step_index = 0;

static uint8_t adpcm_encode(int16_t sample) {
  int32_t step = STEP_TABLE[step_index];
  int32_t diff = sample - predictor;
  uint8_t code = 0;
  if (diff < 0) {
    code = 8;
    diff = -diff;
  }
  int32_t delta = step >> 3;
  if (diff >= step) {
    code |= 4;
    diff -= step;
    delta += step;
  }
  if (diff >= step >> 1) {
    code |= 2;
    diff -= step >> 1;
    delta += step >> 1;
  }
  if (diff >= step >> 2) {
    code |= 1;
    delta += step >> 2;
  }
  predictor = constrain(predictor + ((code & 8) ? -delta : delta), -32768, 32767);
  step_index = constrain(step_index + INDEX_TABLE[code & 7], 0, 88);
  return code;
}

static int find_peer(uint16_t handle) {
  for (int i = 0; i < BLE_MAX_PEERS; i++) {
    if (peers[i].used && peers[i].handle == handle) return i;
  }
  return -1;
}

// 사용 중인 PC 를 바꾼다 (-1 이면 없음). peers_mux 를 잡은 채로 부른다.
static void set_primary_locked(int i) {
  primary = i;
  if (i >= 0) {
    primary_handle = peers[i].handle;
    mtu = peers[i].mtu;
    connected = true;
  } else {
    primary_handle = BLE_HS_CONN_HANDLE_NONE;
    mtu = 23;
    connected = false;
  }
}

static void restart_advertising() {
  // 한 대가 붙으면 광고가 멈추므로, 자리가 남아 있으면 다시 알려 다른 PC 도 찾을 수 있게 한다
  if (enabled && server->getConnectedCount() < BLE_MAX_PEERS) NimBLEDevice::startAdvertising();
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, NimBLEConnInfo &info) override {
    // 소리가 끊기지 않도록 통신 간격을 짧게(7.5~15ms) 요청한다
    srv->updateConnParams(info.getConnHandle(), 6, 12, 0, 200);
    // 연결마다 송신 세기도 가장 세게 (연결 번호별로 따로 정해진다)
    esp_ble_tx_power_set((esp_ble_power_type_t)(ESP_BLE_PWR_TYPE_CONN_HDL0 + (info.getConnHandle() & 7)), ESP_PWR_LVL_P20);
    std::string addr = info.getAddress().toString();
    int slot = -1;
    portENTER_CRITICAL(&peers_mux);
    for (int i = 0; i < BLE_MAX_PEERS && slot < 0; i++) {
      if (!peers[i].used) slot = i;
    }
    if (slot >= 0) {
      Peer &p = peers[slot];
      p.used = true;
      p.handle = info.getConnHandle();
      // Windows 는 연결 알림보다 MTU 협상이 먼저 끝나기도 해서, 지금 값을 직접 읽는다
      p.mtu = info.getMTU();
      strlcpy(p.addr, addr.c_str(), sizeof(p.addr));
      p.host[0] = 0;
      // 사용 중으로 고르는 것은 인증한 뒤에 (handle_line)
      p.hello = p.authed = false;
      p.rx_last = p.tx_ctr = 0;
      p.since_ms = millis();
    }
    portEXIT_CRITICAL(&peers_mux);
    if (slot >= 0) peers[slot].message.clear();
    Serial.printf("[ble] connected %s (slot %d, %u PCs)\n", addr.c_str(), slot, srv->getConnectedCount());
    restart_advertising();
  }
  void onDisconnect(NimBLEServer *srv, NimBLEConnInfo &info, int reason) override {
    int slot;
    portENTER_CRITICAL(&peers_mux);
    slot = find_peer(info.getConnHandle());
    if (slot >= 0) {
      peers[slot].used = false;
      if (primary == slot) {
        // 기다리던 PC 가 있으면 이어받는다 (골라 둔 PC 가 있으면 그쪽)
        int next = -1;
        for (int i = 0; i < BLE_MAX_PEERS; i++) {
          if (!peers[i].used || !peers[i].authed) continue;
          if (next < 0 || strcmp(peers[i].addr, preferred) == 0) next = i;
        }
        set_primary_locked(next);
      }
    }
    portEXIT_CRITICAL(&peers_mux);
    if (slot >= 0) peers[slot].message.clear();
    Serial.printf("[ble] disconnected slot %d (%u PCs left)\n", slot, srv->getConnectedCount());
    restart_advertising();
  }
  void onMTUChange(uint16_t value, NimBLEConnInfo &info) override {
    portENTER_CRITICAL(&peers_mux);
    int slot = find_peer(info.getConnHandle());
    if (slot >= 0) {
      peers[slot].mtu = value;
      if (slot == primary) mtu = value;
    }
    portEXIT_CRITICAL(&peers_mux);
    Serial.printf("[ble] mtu %u (slot %d)\n", value, slot);
  }
};

// PC 도우미는 메시지마다 {"host": "PC 이름"} 을 넣는다. 설정 화면에 주소 대신 이름을 보여준다.
static void remember_host(Peer &p) {
  size_t at = p.message.find("\"host\":\"");
  if (at == std::string::npos) return;
  at += 8;
  size_t end = p.message.find('"', at);
  if (end == std::string::npos) return;
  char host[sizeof(p.host)];
  size_t n = min(end - at, sizeof(host) - 1);
  memcpy(host, p.message.data() + at, n);
  host[n] = 0;
  portENTER_CRITICAL(&peers_mux);
  strlcpy(p.host, host, sizeof(p.host));
  portEXIT_CRITICAL(&peers_mux);
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

// {"ble_hello":"<도전값>"} 에 답한다 (ble_link.h 의 2)
static void answer_hello(int slot) {
  Peer &p = peers[slot];
  const char *m = p.message.c_str();
  size_t pre = strlen(HELLO);
  if (p.message.size() < pre + 32 || !from_hex(m + pre, 16, p.pc_nonce)) return;
  esp_fill_random(p.dev_nonce, sizeof(p.dev_nonce));
  uint8_t tag[32];
  char reply[128];
  if (!net_ble_mac("ble-dev", p.pc_nonce, 16, p.dev_nonce, 16, "", 0, tag)) {
    snprintf(reply, sizeof(reply), "{\"ble_auth\":{\"err\":\"unpaired\"}}");
    refused = true;
  } else {
    char n[33], t[TAG_LEN * 2 + 1];
    to_hex(p.dev_nonce, 16, n);
    to_hex(tag, TAG_LEN, t);
    snprintf(reply, sizeof(reply), "{\"ble_auth\":{\"id\":\"%s\",\"n\":\"%s\",\"t\":\"%s\"}}",
             net_device_id().c_str(), n, t);
  }
  portENTER_CRITICAL(&peers_mux);
  p.hello = true;
  p.authed = false;
  p.rx_last = p.tx_ctr = 0;
  p.since_ms = millis();
  uint16_t peer_mtu = p.mtu;
  portEXIT_CRITICAL(&peers_mux);
  tx->notify((const uint8_t *)reply, strlen(reply), p.handle);
  Serial.printf("[ble] hello from slot %d (mtu %u)\n", slot, (unsigned)peer_mtu);
}

// "#<순번>.<서명>.<JSON>" 을 확인하고 JSON 만 남긴다. 틀리면 false (ble_link.h 의 3)
static bool open_line(Peer &p) {
  const char *m = p.message.c_str();
  if (!p.hello || m[0] != '#') return false;
  char *end;
  unsigned long ctr = strtoul(m + 1, &end, 10);
  if (*end != '.' || ctr == 0 || ctr > 0xFFFFFFFFUL || ctr <= p.rx_last) return false;
  const char *tag_hex = end + 1;
  size_t head = tag_hex + TAG_LEN * 2 + 1 - m;
  uint8_t tag[TAG_LEN], want[32];
  if (p.message.size() < head || m[head - 1] != '.' || !from_hex(tag_hex, TAG_LEN, tag)) return false;
  uint32_t c = ctr;
  if (!net_ble_mac("ble", p.dev_nonce, 16, &c, 4, m + head, p.message.size() - head, want)) return false;
  uint8_t diff = 0;
  for (int k = 0; k < TAG_LEN; k++) diff |= tag[k] ^ want[k];
  if (diff) return false;
  p.rx_last = c;
  p.message.erase(0, head);
  return true;
}

static void handle_line(int slot) {
  Peer &p = peers[slot];
  if (p.message.compare(0, strlen(HELLO), HELLO) == 0) {
    answer_hello(slot);
    return;
  }
  if (!open_line(p)) {
    static uint32_t warned_ms = 0;
    refused = true;
    if (!warned_ms || millis() - warned_ms > 60000) {
      warned_ms = millis();
      Serial.printf("[ble] refused unsigned/bad message from slot %d (%s)\n", slot, p.addr);
    }
    return;
  }
  if (!p.authed) {
    // 처음 인증된 PC: 사용 중인 PC 가 없거나, 설정에서 골라 둔 PC 면 이 PC 를 쓴다
    portENTER_CRITICAL(&peers_mux);
    p.authed = true;
    if (primary < 0 || strcmp(p.addr, preferred) == 0) set_primary_locked(slot);
    portEXIT_CRITICAL(&peers_mux);
    Serial.printf("[ble] slot %d authenticated%s\n", slot, primary == slot ? " - in use" : "");
  }
  remember_host(p);
  // 실제 연결 세대를 큐에 남겨, 처리 전에 선택 PC 가 바뀌면 옛 명령을 거절한다
  uint64_t cookie;
  memcpy(&cookie, p.pc_nonce, sizeof(cookie));
  cookie |= 1;
  String reply;
  if (!link_submit(LINK_BLE, p.message.data(), p.message.size(), slot == primary, &reply, cookie)) {
    ble_send_event("{\"link_error\":\"not_queued\"}", slot);
  } else if (reply.length()) {
    ble_send_event(reply.c_str(), slot);
  }
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
    int slot = find_peer(info.getConnHandle());
    if (slot < 0) return;
    Peer &p = peers[slot];
    NimBLEAttValue value = chr->getValue();
    for (size_t i = 0; i < value.size(); i++) {
      char c = value.data()[i];
      if (c == '\n') {
        if (!p.message.empty()) handle_line(slot);
        p.message.clear();
      } else if (p.message.size() < MAX_MESSAGE) {
        p.message += c;
      }
    }
  }
};

uint64_t ble_primary_cookie() {
  uint64_t cookie = 0;
  portENTER_CRITICAL(&peers_mux);
  if (primary >= 0 && peers[primary].authed && peers[primary].used) {
    memcpy(&cookie, peers[primary].pc_nonce, sizeof(cookie));
    cookie |= 1;
  }
  portEXIT_CRITICAL(&peers_mux);
  return cookie;
}

void ble_link_loop() {
  // 인증하지 않고 자리만 차지하는 연결은 끊는다 (자리가 3개뿐이라 근처의 누가 다 채우면 내 PC 가 못 붙는다)
  for (int i = 0; i < BLE_MAX_PEERS; i++) {
    portENTER_CRITICAL(&peers_mux);
    bool stale = peers[i].used && !peers[i].authed && millis() - peers[i].since_ms > AUTH_TIMEOUT_MS;
    uint16_t handle = peers[i].handle;
    if (stale) peers[i].since_ms = millis();  // 끊기를 기다리는 동안 다시 부르지 않게
    portEXIT_CRITICAL(&peers_mux);
    if (stale && server) {
      Serial.printf("[ble] slot %d did not authenticate, disconnecting\n", i);
      server->disconnect(handle);
    }
  }
  if (refused) {
    refused = false;
    static uint32_t toast_ms = 0;
    if (!toast_ms || millis() - toast_ms > 600000) {
      toast_ms = millis();
      ui_toast("블루투스 연결을 거절했습니다\nPC 도우미를 업데이트하고\nUSB로 한 번 연결하세요", 6000);
    }
  }
}

void ble_link_init() {
  prefs.begin("llm-deck");
  strlcpy(preferred, prefs.getString("ble_pref", "").c_str(), sizeof(preferred));
  NimBLEDevice::init(BLE_NAME);
  // 회사처럼 PC 가 멀리 있어도 찾을 수 있게 송신 세기를 가장 세게(+20dBm, 기본 +9dBm) 올린다.
  // 광고(찾기)와 연결 모두. 전기를 조금 더 쓰지만 책상 기기라 괜찮다.
  esp_err_t adv_err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P20);
  esp_err_t def_err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P20);
  Serial.printf("[ble] tx power adv %d (err %d), default %d (err %d)\n", (int)esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_ADV),
                (int)adv_err, (int)esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT), (int)def_err);
  NimBLEDevice::setMTU(255);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService *service = server->createService(NUS_SERVICE);
  tx = service->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *rx = service->createCharacteristic(NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  service->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setName(BLE_NAME);
  adv->enableScanResponse(true);
  if (settings_link_allowed(LINK_BLE)) {
    adv->start();
    Serial.println("[ble] advertising as " BLE_NAME);
  } else {
    enabled = false;
    Serial.println("[ble] off in settings");
  }
}

void ble_send_audio(uint32_t seq, const int16_t *pcm, size_t samples) {
  static uint8_t packet[AUDIO_HEADER + 160];
  size_t size = AUDIO_HEADER + samples / 2;
  // 한 번에 보낼 수 있는 크기(MTU)가 작게 잡힌 연결에서는 보낼 수 없다
  if (!connected || size > sizeof(packet) || size > (size_t)mtu - 3) return;
  // 보내기 칸이 모자라면 이번 20ms 는 버린다 (늦게 보내 봐야 소용없고, 실패 기록만 쌓인다)
  if (os_msys_num_free() < 3) return;

  // 묶음마다 시작 상태를 함께 실어, 중간에 하나가 사라져도 다음 묶음은 정상적으로 풀린다
  packet[0] = seq & 0xFF;
  packet[1] = (seq >> 8) & 0xFF;
  packet[2] = predictor & 0xFF;
  packet[3] = (predictor >> 8) & 0xFF;
  packet[4] = step_index;
  for (size_t i = 0; i < samples; i += 2) {
    uint8_t low = adpcm_encode(pcm[i]);
    uint8_t high = adpcm_encode(pcm[i + 1]);
    packet[AUDIO_HEADER + i / 2] = low | (high << 4);
  }
  tx->notify(packet, size, primary_handle);
}

void ble_send_event(const char *json, int slot) {
  // 서명을 붙인다 (ble_link.h 의 4)
  uint8_t nonce[16];
  uint32_t ctr = 0;
  uint16_t peer_mtu = 23;
  uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
  portENTER_CRITICAL(&peers_mux);
  if (slot < 0) slot = primary;
  if (slot >= 0 && slot < BLE_MAX_PEERS && peers[slot].used && peers[slot].authed) {
    memcpy(nonce, peers[slot].pc_nonce, 16);
    ctr = ++peers[slot].tx_ctr;
    handle = peers[slot].handle;
    peer_mtu = peers[slot].mtu;
  }
  portEXIT_CRITICAL(&peers_mux);
  if (!connected || handle == BLE_HS_CONN_HANDLE_NONE) return;
  char packet[256];
  size_t len = strlen(json);
  if (len + 46 > sizeof(packet)) return;
  memcpy(packet, json, len);
  // 소리 묶음과 길이가 같으면 PC 가 소리로 착각하므로 JSON 끝에 빈칸을 하나 더한다 (서명은 빈칸까지 포함)
  char trailer[48];
  snprintf(trailer, sizeof(trailer), "|%u.", (unsigned)ctr);
  if (len + strlen(trailer) + TAG_LEN * 2 == AUDIO_HEADER + 160) packet[len++] = ' ';
  uint8_t tag[32];
  if (!net_ble_mac("ble-evt", nonce, 16, &ctr, 4, packet, len, tag)) return;
  size_t size = len + snprintf(packet + len, sizeof(packet) - len, "%s", trailer);
  to_hex(tag, TAG_LEN, packet + size);
  size += TAG_LEN * 2;
  if (size > (size_t)peer_mtu - 3) return;
  // 알림은 짧고 드물지만 꼭 가야 하므로 칸이 빌 때까지 잠깐 기다린다
  for (int i = 0; i < 50 && os_msys_num_free() < 2; i++) vTaskDelay(pdMS_TO_TICKS(4));
  tx->notify((const uint8_t *)packet, size, handle);
}

size_t ble_photo_chunk_max() {
  if (!connected || mtu < 30) return 0;
  size_t packet = min((size_t)mtu - 3, (size_t)240);
  // 소리 묶음(AUDIO_HEADER + 160)과 같은 길이면 PC 가 소리로 착각하므로 한 바이트 줄인다
  if (packet == AUDIO_HEADER + 160) packet--;
  return packet - 4;
}

bool ble_send_photo_chunk(uint32_t offset, const uint8_t *data, size_t len) {
  static uint8_t packet[244];
  if (len + 4 > sizeof(packet)) return false;
  packet[0] = 0xFF;
  packet[1] = offset & 0xFF;
  packet[2] = (offset >> 8) & 0xFF;
  packet[3] = (offset >> 16) & 0xFF;
  memcpy(packet + 4, data, len);
  // 블루투스 보내기 칸(mbuf)이 다 차면 notify 가 "ENOMEM" 오류로 실패한다. PC 가 받아 가는 속도는
  // 연결 간격에 달려 있어(Windows 는 Mac 보다 느리다) 칸이 비기를 먼저 기다렸다가 보낸다.
  // 마이크 소리와, PC 가 보내오는 것(사용량·명령)을 받을 자리로 몇 칸은 남겨 둔다.
  // (다 쓰면 PC 의 쓰기가 "Insufficient Resource" 로 거절되어 PC 쪽 연결이 끊긴다)
  uint32_t start = millis();
  while (millis() - start < 10000) {
    if (!connected) return false;
    if (os_msys_num_free() >= 10 && tx->notify(packet, len + 4, primary_handle)) return true;
    vTaskDelay(pdMS_TO_TICKS(4));
  }
  Serial.println("[ble] photo chunk timed out (PC not reading)");
  return false;
}

bool ble_link_connected() {
  return connected;
}

void ble_link_set_enabled(bool on) {
  if (on == enabled || !server) return;
  enabled = on;
  if (on) {
    NimBLEDevice::startAdvertising();
    Serial.println("[ble] on");
  } else {
    NimBLEDevice::stopAdvertising();
    for (uint16_t handle : server->getPeerDevices()) server->disconnect(handle);
    Serial.println("[ble] off");
  }
}

bool ble_link_enabled() {
  return enabled;
}

int ble_peer_get(int slot, char *name, size_t size) {
  if (slot < 0 || slot >= BLE_MAX_PEERS) return 0;
  portENTER_CRITICAL(&peers_mux);
  const Peer &p = peers[slot];
  int state = !p.used ? 0 : slot == primary ? 2 : 1;
  if (p.used && name && size) strlcpy(name, p.host[0] ? p.host : p.addr, size);
  portEXIT_CRITICAL(&peers_mux);
  return state;
}

int ble_peer_count() {
  return server ? server->getConnectedCount() : 0;
}

void ble_peer_select(int slot) {
  if (slot < 0 || slot >= BLE_MAX_PEERS) return;
  portENTER_CRITICAL(&peers_mux);
  bool ok = peers[slot].used && peers[slot].authed;  // 인증한 PC 만 고를 수 있다
  if (ok) {
    set_primary_locked(slot);
    strlcpy(preferred, peers[slot].addr, sizeof(preferred));
  }
  portEXIT_CRITICAL(&peers_mux);
  if (!ok) return;
  prefs.putString("ble_pref", preferred);
  Serial.printf("[ble] slot %d in use (%s)\n", slot, preferred);
}

void ble_peer_disconnect(int slot) {
  if (slot < 0 || slot >= BLE_MAX_PEERS || !server) return;
  portENTER_CRITICAL(&peers_mux);
  bool used = peers[slot].used;
  uint16_t handle = peers[slot].handle;
  portEXIT_CRITICAL(&peers_mux);
  if (!used) return;
  server->disconnect(handle);
  Serial.printf("[ble] slot %d disconnected from settings\n", slot);
}

// 블루투스 스피커 연결 시험. 스피커 음악은 보통 "블루투스 클래식(A2DP)" 으로 가는데, 칩마다 지원이 다르다.
// 1) 칩이 클래식·BLE 를 지원하는지 칩 정보로 확인하고, 2) 10초 동안 주변 BLE 기기를 모두 적는다.
// 스피커로 보이는 것(모양 정보가 "오디오 출력")과 LE Audio(BLE 로 소리 보내기) 서비스를 가진 것을 표시한다.
// 3) 대상(주소, 또는 "all" 이면 연결할 수 있는 기기 모두)에 연결해 그 기기가 BLE 로 받는 서비스 목록을 읽는다.
static volatile bool checking = false;
static char check_target[20] = "";
#define CHECK_MAX_PROBE 8

// 자주 보이는 서비스 이름 (16비트 번호). 소리를 받는 통로가 있는지 알아보기 쉽게 한다.
static const char *service_name(const NimBLEUUID &uuid) {
  if (uuid.bitSize() != 16) return "vendor (custom)";
  switch (*(const uint16_t *)uuid.getValue()) {
    case 0x1800: return "Generic Access";
    case 0x1801: return "Generic Attribute";
    case 0x180A: return "Device Information";
    case 0x180F: return "Battery";
    case 0x1812: return "HID (buttons)";
    case 0x1844: return "Volume Control";
    case 0x1845: return "Volume Offset";
    case 0x1846: return "Coordinated Set";
    case 0x184D: return "Microphone Control";
    case 0x184E: return "LE Audio: Audio Stream Control  <- receives audio";
    case 0x184F: return "LE Audio: Broadcast Audio Scan  <- receives audio";
    case 0x1850: return "LE Audio: Published Audio Capabilities";
    case 0x1853: return "LE Audio: Common Audio";
    case 0x1855: return "Telephony and Media Audio";
    case 0x1848: return "Media Control";
    case 0xFE2C: return "Google Fast Pair";
    default: return "";
  }
}

static std::string printable(const std::string &in) {
  std::string out = in;
  for (char &c : out) {
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) c = '?';  // PC 로그에 글자로 보이게
  }
  return out;
}

// 연결해서 서비스·특성 목록을 적는다. 읽을 수 있는 짧은 값(이름·제조사·모델 등)은 내용도 적는다.
static void probe_services(const NimBLEAdvertisedDevice *d) {
  Serial.printf("[bt-check] --- connecting to %s (%s) ---\n", d->getAddress().toString().c_str(),
                d->getName().empty() ? "no name" : printable(d->getName()).c_str());
  NimBLEClient *client = NimBLEDevice::createClient();
  if (!client) {
    Serial.println("[bt-check]   no free connection slot");
    return;
  }
  client->setConnectTimeout(8000);
  if (!client->connect(d)) {
    Serial.println("[bt-check]   connect failed");
    NimBLEDevice::deleteClient(client);
    return;
  }
  const std::vector<NimBLERemoteService *> &services = client->getServices(true);
  Serial.printf("[bt-check]   connected, %u services\n", (unsigned)services.size());
  bool audio = false;
  for (NimBLERemoteService *svc : services) {
    const char *name = service_name(svc->getUUID());
    audio = audio || strstr(name, "receives audio");
    Serial.printf("[bt-check]   service %s  %s\n", svc->getUUID().toString().c_str(), name);
    for (NimBLERemoteCharacteristic *chr : svc->getCharacteristics(true)) {
      char props[24] = "";
      if (chr->canRead()) strcat(props, "read ");
      if (chr->canWrite()) strcat(props, "write ");
      if (chr->canWriteNoResponse()) strcat(props, "write-nr ");
      if (chr->canNotify()) strcat(props, "notify ");
      if (chr->canIndicate()) strcat(props, "indicate");
      std::string value;
      if (chr->canRead()) {
        NimBLEAttValue v = chr->readValue();
        value.assign((const char *)v.data(), min((size_t)v.size(), (size_t)32));
      }
      Serial.printf("[bt-check]     char %s  [%s]%s%s\n", chr->getUUID().toString().c_str(), props,
                    value.empty() ? "" : "  = ", printable(value).c_str());
    }
  }
  Serial.printf("[bt-check]   audio input over BLE: %s\n", audio ? "YES (LE Audio)" : "none");
  client->disconnect();
  NimBLEDevice::deleteClient(client);
}

static void speaker_check_task(void *) {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  Serial.printf("[bt-check] chip %s rev %d: Bluetooth Classic (A2DP speakers) %s, BLE %s\n", CONFIG_IDF_TARGET,
                chip.revision / 100, (chip.features & CHIP_FEATURE_BT) ? "yes" : "NO",
                (chip.features & CHIP_FEATURE_BLE) ? "yes" : "no");
  Serial.println("[bt-check] scanning BLE for 10 s...");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(10000, false);
  // LE Audio 를 받는 쪽(스피커)이 광고하는 서비스: 0x184E 소리 스트림 제어, 0x184F 방송 소리 찾기, 0x1850 지원 소리 형식,
  // 0x1853 공통 오디오. (0x1852 는 보내는 쪽의 방송 알림이라 넣지 않는다)
  // 주의: 이 빌드는 NimBLE 확장 광고(CONFIG_BT_NIMBLE_EXT_ADV)를 켜지 않아 예전 방식 광고만 받는다.
  // LE Audio 스피커는 확장 광고를 쓰므로, 여기서 안 보여도 LE Audio 가 없다고 확정할 수는 없다.
  static const uint16_t LE_AUDIO[] = {0x184E, 0x184F, 0x1850, 0x1853};
  int speakers = 0, le_audio = 0;
  const NimBLEAdvertisedDevice *probe[CHECK_MAX_PROBE];
  int probes = 0;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice *d = results.getDevice(i);
    // 모양 정보 0x0840~0x087F 는 "오디오 출력"(스피커·사운드바 등)
    bool speaker = d->haveAppearance() && (d->getAppearance() >> 6) == 0x21;
    bool audio = false;
    for (uint16_t uuid : LE_AUDIO) audio = audio || d->isAdvertisingService(NimBLEUUID(uuid));
    speakers += speaker;
    le_audio += audio;
    // 제조사 번호 (0x004C 애플, 0x0006 마이크로소프트, 0x0075 삼성 ...): 이름이 없는 기기를 알아보는 실마리
    char maker[12] = "";
    if (d->getManufacturerDataCount()) {
      std::string m = d->getManufacturerData();
      if (m.size() >= 2) snprintf(maker, sizeof(maker), " mfr %02X%02X", (uint8_t)m[1], (uint8_t)m[0]);
    }
    char uuids[48] = "";
    for (int u = 0; u < d->getServiceUUIDCount() && strlen(uuids) < 36; u++) {
      strlcat(uuids, " ", sizeof(uuids));
      strlcat(uuids, d->getServiceUUID(u).toString().c_str(), sizeof(uuids));
    }
    std::string name = printable(d->getName());
    Serial.printf("[bt-check]  %4d dBm  %s  %s%s%s%s%s%s\n", d->getRSSI(), d->getAddress().toString().c_str(),
                  name.empty() ? "(no name)" : name.c_str(), d->isConnectable() ? "  connectable" : "", maker, uuids,
                  speaker ? "  [speaker]" : "", audio ? "  [LE Audio]" : "");
    bool wanted = strcmp(check_target, "all") == 0 ? d->isConnectable()
                                                    : strcasecmp(check_target, d->getAddress().toString().c_str()) == 0;
    if (wanted && probes < CHECK_MAX_PROBE) probe[probes++] = d;
  }
  Serial.printf("[bt-check] done: %d BLE devices, %d look like speakers, %d offer LE Audio\n", results.getCount(),
                speakers, le_audio);
  if (check_target[0] && !probes) Serial.printf("[bt-check] %s not found in scan\n", check_target);
  for (int i = 0; i < probes; i++) probe_services(probe[i]);
  if (check_target[0]) Serial.println("[bt-check] services done");
  scan->clearResults();
  checking = false;
  vTaskDelete(nullptr);
}

void ble_speaker_check(const char *target) {
  if (checking || !server) return;
  checking = true;
  strlcpy(check_target, target ? target : "", sizeof(check_target));
  xTaskCreate(speaker_check_task, "bt_check", 8192, nullptr, 1, nullptr);
}
