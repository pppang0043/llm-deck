#include <Arduino.h>
#include <Preferences.h>
#include <time.h>

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

// 화면 방향 = 기울기로 알아낸 방향 + 버튼으로 더 돌린 횟수.
// 버튼으로 돌린 횟수는 저장해 두므로, 기울기 방향이 맞지 않으면 버튼으로 한 번 맞춰 두면 된다.
static Preferences prefs;
static uint8_t rotate_offset = 0;
static int tilt = 0;

static void apply_rotation() {
  uint8_t rotation = (tilt + rotate_offset) & 3;
  if (rotation == display_rotation()) return;
  display_set_rotation(rotation);
  ui_layout();
  Serial.printf("[main] screen rotation %d (tilt %d + button %d)\n", rotation, tilt, rotate_offset);
}

static void rotate_by_button() {
  rotate_offset = (rotate_offset + 1) & 3;
  prefs.putUChar("rot_offset", rotate_offset);
  apply_rotation();
}

// 기기를 돌려 세우면 화면도 따라 돈다. 같은 방향이 잠깐 유지되어야 바꾼다 (흔들림에 반응하지 않도록).
// 눕혀 놓았거나 애매하게 기울어 있으면 마지막 방향을 유지한다.
static void follow_tilt() {
  static uint32_t checked_ms = 0;
  static int candidate = -1;
  static uint8_t count = 0;

  if (millis() - checked_ms < 150) return;
  checked_ms = millis();
  int now = imu_rotation();
  if (now < 0 || now == tilt) {
    candidate = -1;
    return;
  }
  count = now == candidate ? count + 1 : 1;
  candidate = now;
  if (count >= 4) {
    tilt = now;
    candidate = -1;
    apply_rotation();
  }
}

// 상태줄의 날짜와 시각. 시각은 Wi-Fi(인터넷) 또는 PC 도우미에게서 받으며, 받기 전에는 비워 둔다.
static void show_clock() {
  static uint32_t checked_ms = 0;
  if (millis() - checked_ms < 1000) return;
  checked_ms = millis();

  char text[32] = "";
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm local;
    localtime_r(&now, &local);
    snprintf(text, sizeof(text), "%d월 %d일 %02d:%02d", local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
  }
  ui_set_clock(text);
}

void setup() {
  usb_link_init();
  Serial.begin(115200);
  pinMode(BTN_BOOT, INPUT_PULLUP);

  display_init();
  ui_init(toggle_mic, rotate_by_button);
  imu_init();
  prefs.begin("llm-deck");
  rotate_offset = prefs.getUChar("rot_offset", 0) & 3;
  delay(50);  // 센서가 첫 값을 낼 때까지 잠깐 기다린다
  int start = imu_rotation();
  if (start >= 0) tilt = start;
  float ax = 0, ay = 0, az = 0;
  imu_read(&ax, &ay, &az);
  Serial.printf("[main] tilt %d (accel %.2f %.2f %.2f), button %d\n", start, ax, ay, az, rotate_offset);
  apply_rotation();
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
  show_clock();

  if (mic_is_on() && millis() - level_ms > 50) {
    level_ms = millis();
    ui_set_level(mic_level());
  }
  delay(5);
}
