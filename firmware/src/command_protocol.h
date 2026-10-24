#pragma once
#include <ArduinoJson.h>
#include <string.h>

// 이 목록은 companion/transports/commands.py 와 맞춘다. 주기적인 상태/터치는
// 플래시에 기록하지 않는다. 명시적인 제어 명령만 영구 중복 방지 대상으로 삼는다.
static const char *const COMMAND_FIELDS[] = {
  "mic", "battery_mv", "camera", "view", "camera_mount", "saver", "i2c_test", "usb_reconnect",
  "rotate", "photo_test", "wifi_add", "wifi_clear", "ble_use", "bt_check", "sd_pull",
  "health_clear", "hang_test", "crash_test", "sd_clear", "screenshot", "links", "camera_ctl"
};

inline bool valid_command(JsonObjectConst body) {
  if (body.isNull() || body.size() == 0) return false;
  for (JsonPairConst field : body) {
    const char *key = field.key().c_str();
    JsonVariantConst v = field.value();
    bool known = false;
    for (const char *allowed : COMMAND_FIELDS) if (strcmp(key, allowed) == 0) known = true;
    if (!known) return false;
    if (strcmp(key, "camera") == 0) {
      const char *cmd = v | "";
      if (strcmp(cmd, "open") && strcmp(cmd, "close") && strcmp(cmd, "shoot") && strcmp(cmd, "record") &&
          strcmp(cmd, "dump") && strcmp(cmd, "gallery") && strcmp(cmd, "scan")) return false;
    } else if (strcmp(key, "view") == 0) {
      if (!v.is<const char *>() || strlen(v.as<const char *>()) > 32) return false;
    } else if (strcmp(key, "camera_mount") == 0) {
      if (!v.is<JsonObjectConst>() || !v["rot"].is<int>() || v["rot"].as<int>() < 0 || v["rot"].as<int>() > 3 ||
          (!v["mirror"].isNull() && !v["mirror"].is<bool>())) return false;
    } else if (strcmp(key, "camera_ctl") == 0) {
      // 화질 조절 {"sharp": 2, ...}: 이름과 범위는 기기(camera_ctl_set)가 다시 확인한다
      if (!v.is<JsonObjectConst>() || !v.size() || v.size() > 32) return false;
      for (JsonPairConst c : v.as<JsonObjectConst>()) {
        if (!c.value().is<int>() || strlen(c.key().c_str()) > 12) return false;
      }
    } else if (strcmp(key, "links") == 0) {
      if (!v.is<JsonObjectConst>() || !v.size()) return false;
      for (JsonPairConst link : v.as<JsonObjectConst>()) {
        const char *name = link.key().c_str();
        if (!link.value().is<bool>() || (strcmp(name, "usb") && strcmp(name, "wifi") && strcmp(name, "ble") && strcmp(name, "cloud"))) return false;
      }
    } else if (strcmp(key, "wifi_add") == 0) {
      const char *ssid = v["ssid"] | "", *password = v["password"] | "";
      if (!ssid[0] || strlen(ssid) > 32 || strlen(password) > 64) return false;
    } else if (strcmp(key, "ble_use") == 0 || strcmp(key, "battery_mv") == 0) {
      if (!v.is<int>() || v.as<int>() < 0 || v.as<int>() > (strcmp(key, "ble_use") == 0 ? 2 : 6000)) return false;
    } else if (strcmp(key, "bt_check") == 0) {
      if (v != true && (!v.is<const char *>() || strlen(v.as<const char *>()) > 32)) return false;
    } else if (strcmp(key, "mic") == 0) {
      if (!v.is<bool>()) return false;
    } else if (v != true) return false;
  }
  return true;
}
