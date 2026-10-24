#include "ble_hid.h"

#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>

#include "battery.h"

#define HID_NAME "LLM Deck Keys"
#define REPORT_KEYS 1
#define REPORT_MEDIA 2

// 키보드(보고서 1: 조합 키 1바이트 + 예약 1바이트 + 키 6개)와 미디어 키(보고서 2: 16비트 코드 하나)
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,              // Generic Desktop, Keyboard, Collection(Application)
    0x85, REPORT_KEYS,                               //   Report ID
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,              //   조합 키 (Ctrl, Shift, Alt, GUI)
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,              //   예약
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,  //   키 6개
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,              // Consumer Control
    0x85, REPORT_MEDIA,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

static Preferences prefs;
static int mode = -1;  // -1 모름, 0 끔, 1 켬
static NimBLEServer *server = nullptr;
static NimBLECharacteristic *keys = nullptr, *media = nullptr;

bool ble_hid_mode() {
  if (mode < 0) {
    prefs.begin("ble_hid", true);
    mode = prefs.getBool("on", false) ? 1 : 0;
    prefs.end();
  }
  return mode == 1;
}

void ble_hid_set_mode(bool on) {
  prefs.begin("ble_hid", false);
  prefs.putBool("on", on);
  prefs.end();
  Serial.printf("[hid] keyboard mode %s, restarting\n", on ? "on" : "off");
  delay(100);
  ESP.restart();
}

class HidCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, NimBLEConnInfo &info) override {
    Serial.printf("[hid] connected %s\n", info.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer *srv, NimBLEConnInfo &info, int reason) override {
    Serial.printf("[hid] disconnected (reason %d)\n", reason);
    NimBLEDevice::startAdvertising();  // 다시 찾을 수 있게
  }
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    Serial.printf("[hid] paired: %s\n", info.isEncrypted() ? "encrypted" : "not encrypted");
  }
};

void ble_hid_init() {
  NimBLEDevice::init(HID_NAME);
  // 키보드는 암호화된 연결이 필요하다. 화면·버튼으로 번호를 맞추지 않는 방식(Just Works)으로 페어링하고 기억한다
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  server = NimBLEDevice::createServer();
  server->setCallbacks(new HidCallbacks());
  NimBLEHIDDevice *hid = new NimBLEHIDDevice(server);
  keys = hid->getInputReport(REPORT_KEYS);
  media = hid->getInputReport(REPORT_MEDIA);
  hid->setManufacturer("LLM Deck");
  hid->setPnp(0x02, 0x303A, 0x4C44, 0x0100);  // USB 제조사 번호 방식, Espressif
  hid->setHidInfo(0x00, 0x01);
  hid->setReportMap((uint8_t *)REPORT_MAP, sizeof(REPORT_MAP));
  int pct = battery_pct();
  hid->setBatteryLevel(pct < 0 ? 100 : pct);
  server->start();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(0x03C1);  // 키보드
  adv->addServiceUUID(hid->getHidService()->getUUID());
  adv->setName(HID_NAME);
  adv->enableScanResponse(true);
  adv->start();
  Serial.println("[hid] advertising as " HID_NAME);
}

bool ble_hid_connected() {
  return server && server->getConnectedCount() > 0;
}

bool ble_hid_key(uint8_t keycode, uint8_t modifiers) {
  if (!ble_hid_connected()) return false;
  uint8_t down[8] = {modifiers, 0, keycode, 0, 0, 0, 0, 0};
  uint8_t up[8] = {0};
  keys->setValue(down, sizeof(down));
  keys->notify();
  delay(10);
  keys->setValue(up, sizeof(up));
  keys->notify();
  return true;
}

bool ble_hid_media(uint16_t usage) {
  if (!ble_hid_connected()) return false;
  uint8_t down[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
  uint8_t up[2] = {0, 0};
  media->setValue(down, sizeof(down));
  media->notify();
  delay(10);
  media->setValue(up, sizeof(up));
  media->notify();
  return true;
}
