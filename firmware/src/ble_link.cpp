#include "ble_link.h"

#include <NimBLEDevice.h>
#include <string>

#include "link.h"

#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // PC -> 기기
#define NUS_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // 기기 -> PC

#define MAX_MESSAGE 16384
#define AUDIO_HEADER 5

static NimBLEServer *server;
static NimBLECharacteristic *tx;
static std::string message;
static volatile bool connected = false;
static volatile uint16_t mtu = 23;

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

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, NimBLEConnInfo &info) override {
    connected = true;
    // 소리가 끊기지 않도록 통신 간격을 짧게(7.5~15ms) 요청한다
    srv->updateConnParams(info.getConnHandle(), 6, 12, 0, 200);
    Serial.println("[ble] connected");
  }
  void onDisconnect(NimBLEServer *srv, NimBLEConnInfo &info, int reason) override {
    connected = false;
    mtu = 23;
    message.clear();
    Serial.println("[ble] disconnected");
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t value, NimBLEConnInfo &info) override {
    mtu = value;
    Serial.printf("[ble] mtu %u\n", value);
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
    NimBLEAttValue value = chr->getValue();
    for (size_t i = 0; i < value.size(); i++) {
      char c = value.data()[i];
      if (c == '\n') {
        if (!message.empty()) link_submit(LINK_BLE, message.data(), message.size());
        message.clear();
      } else if (message.size() < MAX_MESSAGE) {
        message += c;
      }
    }
  }
};

void ble_link_init() {
  NimBLEDevice::init(BLE_NAME);
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
  adv->start();
  Serial.println("[ble] advertising as " BLE_NAME);
}

void ble_send_audio(uint32_t seq, const int16_t *pcm, size_t samples) {
  static uint8_t packet[AUDIO_HEADER + 160];
  size_t size = AUDIO_HEADER + samples / 2;
  // 한 번에 보낼 수 있는 크기(MTU)가 작게 잡힌 연결에서는 보낼 수 없다
  if (!connected || size > sizeof(packet) || size > (size_t)mtu - 3) return;

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
  tx->notify(packet, size);
}
