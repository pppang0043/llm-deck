#include <Arduino.h>

#include "display.h"
#include "mic.h"
#include "net.h"
#include "pins.h"
#include "ui.h"

static void toggle_mic() {
  mic_set_on(!mic_is_on());
  ui_set_mic(mic_is_on());
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

void setup() {
  Serial.begin(115200);
  pinMode(BTN_BOOT, INPUT_PULLUP);

  display_init();
  ui_init(toggle_mic);
  mic_init();
  net_init();
  Serial.println("[main] setup done");
}

void loop() {
  static uint32_t level_ms = 0;

  display_loop();
  net_loop();
  if (boot_button_pressed()) toggle_mic();

  if (mic_is_on() && millis() - level_ms > 50) {
    level_ms = millis();
    ui_set_level(mic_level());
  }
  delay(5);
}
