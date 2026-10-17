#include "ble_link.h"

#include <NimBLEDevice.h>
#include <esp_bt.h>
#include <nimble/porting/nimble/include/os/os_mbuf.h>
#include <Preferences.h>
#include <string>

#include "link.h"
#include "settings.h"

#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // PC -> 기기
#define NUS_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // 기기 -> PC

#define MAX_MESSAGE 16384
#define AUDIO_HEADER 5

static NimBLEServer *server;
static NimBLECharacteristic *tx;
// 여러 PC 가 동시에 붙을 수 있다 (최대 BLE_MAX_PEERS). 그중 하나만 "사용 중" 이고, 마이크 소리·사진·알림은
// 그 PC 로만 보내며 화면의 사용량도 그 PC 것을 쓴다. 나머지는 연결만 유지한 채 기다린다(명령은 따른다).
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
      // 사용 중인 PC 가 없거나, 설정에서 골라 둔 PC 가 다시 붙었으면 이 PC 를 쓴다
      if (primary < 0 || strcmp(p.addr, preferred) == 0) set_primary_locked(slot);
    }
    portEXIT_CRITICAL(&peers_mux);
    if (slot >= 0) peers[slot].message.clear();
    Serial.printf("[ble] connected %s (slot %d, %u PCs)%s\n", addr.c_str(), slot, srv->getConnectedCount(),
                  slot >= 0 && primary == slot ? " - in use" : "");
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
          if (!peers[i].used) continue;
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

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
    int slot = find_peer(info.getConnHandle());
    if (slot < 0) return;
    Peer &p = peers[slot];
    NimBLEAttValue value = chr->getValue();
    for (size_t i = 0; i < value.size(); i++) {
      char c = value.data()[i];
      if (c == '\n') {
        if (!p.message.empty()) {
          remember_host(p);
          // 기다리는 PC 가 보낸 것은 명령만 따르고 사용량은 쓰지 않는다
          link_submit(LINK_BLE, p.message.data(), p.message.size(), slot == primary);
        }
        p.message.clear();
      } else if (p.message.size() < MAX_MESSAGE) {
        p.message += c;
      }
    }
  }
};

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

void ble_send_event(const char *json) {
  size_t size = strlen(json);
  if (!connected || size > (size_t)mtu - 3) return;
  // 알림은 짧고 드물지만 꼭 가야 하므로 칸이 빌 때까지 잠깐 기다린다
  for (int i = 0; i < 50 && os_msys_num_free() < 2; i++) vTaskDelay(pdMS_TO_TICKS(4));
  tx->notify((const uint8_t *)json, size, primary_handle);
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
  bool ok = peers[slot].used;
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
