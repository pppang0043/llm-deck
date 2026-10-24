#pragma once
#include "link_types.h"
#include "command_protocol.h"

inline bool has_control(JsonObjectConst body) {
  for (const char *key : COMMAND_FIELDS) if (!body[key].isNull()) return true;
  return !body["touch"].isNull() || !body["mirror"].isNull();
}

inline bool control_allowed(Link source, Link active, bool selected, JsonObjectConst body) {
  if (source == LINK_USB) return true; // 직접 꽂은 PC 의 복구/설정 명령
  if (source != LINK_BLE && source != LINK_WIFI) return false;
  // 대기 PC 를 명시적으로 선택하는 기존 기능은 유지한다. 다른 명령을 섞으면 안 된다.
  if (source == LINK_BLE && body.size() == 1 && body["ble_use"].is<int>()) return true;
  return selected && (active == source || active == LINK_NONE || active == LINK_CLOUD);
}

// HTTP 는 연결을 유지하지 않으므로 인증된 IP 하나를 같은 시간 규칙으로 유지한다.
// 다른 PC 의 수신 데이터가 마이크/사진 목적지와 소유자 수명을 바꾸지 않게 한다.
class WifiPeerLease {
 public:
  bool claim(uint32_t peer, uint32_t now, bool heartbeat, bool legacy) {
    if (!peer) return false;
    if (peer_ && peer_ != peer && uint32_t(now - seen_) <= lease_) return false;
    if (peer_ != peer) lease_ = 75000;
    peer_ = peer;
    seen_ = now;
    if (heartbeat) lease_ = 15000;
    if (legacy) lease_ = 75000;
    return true;
  }
  bool owns(uint32_t peer, uint32_t now) const {
    return peer && peer == peer_ && uint32_t(now - seen_) <= lease_;
  }
  void reset() { peer_ = 0; }
 private:
  uint32_t peer_ = 0, seen_ = 0, lease_ = 75000;
};
