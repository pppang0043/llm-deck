#include <Arduino.h>

#include "ble_link.h"
#include "cloud.h"
#include "display.h"
#include "imu.h"
#include "link.h"
#include "mic.h"
#include "net.h"
#include "pins.h"
#include "ui.h"
#include "usb_link.h"

static void set_mic(bool on) {
  mic_set_on(on);
  ui_set_mic(on);
}

static void toggle_mic() {
  set_mic(!mic_is_on());
}

// BOOT 버튼이 눌린 순간을 한 번만 잡아낸다 (떨림 방지 30ms)
static bool boot_button_pressed() {
  static bool stable = HIGH;
  static bool last = HIGH;
  static uint32_t changed_ms = 0;

  bool now = digitalRead(BTN_BOOT);
  if (now != last) {
    last = now;
    changed_ms = millis();
  }
  if (now != stable && millis() - changed_ms > 30) {
    stable = now;
    return stable == LOW;
  }
  return false;
}

// 기기를 돌려 세우면 화면도 따라 돈다. 같은 방향이 잠깐 유지되어야 바꾼다 (흔들림에 반응하지 않도록).
static void follow_tilt() {
  static uint32_t checked_ms = 0;
  static int candidate = -1;
  static uint8_t count = 0;

  if (millis() - checked_ms < 150) return;
  checked_ms = millis();
  int rotation = imu_rotation();
  if (rotation < 0 || rotation == display_rotation()) {
    candidate = -1;
    return;
  }
  count = rotation == candidate ? count + 1 : 1;
  candidate = rotation;
  if (count >= 4) {
    display_set_rotation(rotation);
    ui_layout();
    Serial.printf("[main] screen rotation %d\n", rotation);
    candidate = -1;
  }
}

void setup() {
  usb_link_init();
  Serial.begin(115200);
  pinMode(BTN_BOOT, INPUT_PULLUP);

  display_init();
  ui_init(toggle_mic);
  imu_init();
  link_init(set_mic);
  mic_init();
  net_init();
  ble_link_init();
  cloud_init();
  Serial.println("[main] setup done");
}

void loop() {
  static uint32_t level_ms = 0;

  display_loop();
  usb_link_loop();
  net_loop();
  link_loop();
  if (boot_button_pressed()) toggle_mic();
  follow_tilt();

  if (mic_is_on() && millis() - level_ms > 50) {
    level_ms = millis();
    ui_set_level(mic_level());
  }
  delay(5);
}
