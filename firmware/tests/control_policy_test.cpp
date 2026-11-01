#include "control_policy.h"
#include <assert.h>

int main() {
  JsonDocument doc;
  assert(!deserializeJson(doc, "{\"camera\":\"record\"}"));
  JsonObjectConst body = doc.as<JsonObjectConst>();
  assert(valid_command(body) && has_control(body));
  assert(control_allowed(LINK_BLE, LINK_BLE, true, body));
  assert(!control_allowed(LINK_BLE, LINK_BLE, false, body));
  // 수신 뒤 USB 로 제어권이 넘어가면 같은 큐 항목도 실행을 거절한다.
  assert(!control_allowed(LINK_BLE, LINK_USB, true, body));
  assert(!control_allowed(LINK_WIFI, LINK_WIFI, false, body));
  assert(control_allowed(LINK_WIFI, LINK_WIFI, true, body));
  assert(!control_allowed(LINK_WIFI, LINK_USB, true, body));
  assert(control_allowed(LINK_USB, LINK_WIFI, true, body));
  assert(control_allowed(LINK_WIFI, LINK_NONE, true, body));
  assert(!control_allowed(LINK_CLOUD, LINK_NONE, true, body));

  assert(!deserializeJson(doc, "{\"ble_use\":1}"));
  assert(control_allowed(LINK_BLE, LINK_USB, false, doc.as<JsonObjectConst>()));
  assert(!deserializeJson(doc, "{\"ble_use\":1,\"camera\":\"record\"}"));
  assert(!control_allowed(LINK_BLE, LINK_USB, false, doc.as<JsonObjectConst>()));
  assert(!deserializeJson(doc, "{\"touch\":{\"x\":1,\"down\":true}}"));
  assert(has_control(doc.as<JsonObjectConst>()));
  assert(!control_allowed(LINK_WIFI, LINK_USB, true, doc.as<JsonObjectConst>()));
  assert(!deserializeJson(doc, "{\"services\":[],\"host\":\"standby\"}"));
  assert(!has_control(doc.as<JsonObjectConst>()));

  // 상태만 담긴 메시지: 사용 중인 PC 것만 실행한다
  assert(message_allowed(LINK_WIFI, LINK_USB, true, doc.as<JsonObjectConst>()));
  assert(!message_allowed(LINK_BLE, LINK_BLE, false, doc.as<JsonObjectConst>()));
  // 제어 명령: 수신 뒤 제어권이 넘어갔으면 사용 중이라도 거절한다
  assert(!deserializeJson(doc, "{\"camera\":\"shoot\"}"));
  assert(message_allowed(LINK_WIFI, LINK_WIFI, true, doc.as<JsonObjectConst>()));
  assert(!message_allowed(LINK_WIFI, LINK_USB, true, doc.as<JsonObjectConst>()));
  assert(message_allowed(LINK_USB, LINK_WIFI, false, doc.as<JsonObjectConst>()));
  // 대기 BLE PC 의 선택 명령은 사용 중이 아니어도 받는다
  assert(!deserializeJson(doc, "{\"ble_use\":1}"));
  assert(message_allowed(LINK_BLE, LINK_USB, false, doc.as<JsonObjectConst>()));

  // USB 전용 명령
  for (const char *text : {"{\"links\":{\"ble\":false}}", "{\"sd_pull\":true}", "{\"sd_clear\":true}",
                           "{\"health_clear\":true}", "{\"hang_test\":true}", "{\"crash_test\":true}",
                           "{\"wifi_add\":{\"ssid\":\"x\"}}", "{\"wifi_clear\":true}"}) {
    assert(!deserializeJson(doc, text));
    assert(usb_only_command(doc.as<JsonObjectConst>()));
  }
  for (const char *text : {"{\"camera\":\"shoot\"}", "{\"view\":\"home\"}", "{\"screenshot\":true}", "{\"mic\":true}"}) {
    assert(!deserializeJson(doc, text));
    assert(!usb_only_command(doc.as<JsonObjectConst>()));
  }

  // 옛 도우미 구별
  assert(!deserializeJson(doc, "{\"helper\":{\"proto\":3}}"));
  assert(legacy_helper(doc.as<JsonObjectConst>()));
  assert(!deserializeJson(doc, "{\"helper\":{\"proto\":4}}"));
  assert(!legacy_helper(doc.as<JsonObjectConst>()));
  assert(!deserializeJson(doc, "{\"services\":[]}"));
  assert(!legacy_helper(doc.as<JsonObjectConst>()));

  WifiPeerLease owner;
  assert(owner.claim(1, 0, true, false));
  assert(!owner.claim(2, 14999, true, false));
  assert(owner.owns(1, 15000)); // 거절된 PC 가 소유자 수명을 늘리지 않는다
  assert(!owner.owns(1, 15001));
  assert(owner.claim(2, 15001, true, false));
  assert(!owner.owns(1, 15002) && owner.owns(2, 15002));
  owner.reset();
  assert(!owner.owns(2, 15003));
  assert(owner.claim(3, 0xfffffff0U, true, false));
  assert(owner.owns(3, 1000)); // millis 의 32비트 순환
  assert(!owner.owns(3, 20000));
  owner.reset();
  assert(owner.claim(4, 1, false, true));
  assert(owner.owns(4, 30001)); // 이전 도우미의 30초 전송도 유지
  assert(!owner.claim(5, 74000, true, false));
  assert(owner.claim(5, 75002, true, false));
}
