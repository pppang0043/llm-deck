#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

#include "battery.h"
#include "ble_link.h"
#include "camera.h"
#include "camera_ui.h"
#include "cloud.h"
#include "display.h"
#include "imu.h"
#include "link.h"
#include "mic.h"
#include "net.h"
#include "photo.h"
#include "saver.h"
#include "sd_video.h"
#include "settings.h"
#include "askpanel.h"
#include "health.h"
#include "history.h"
#include "pcpanel.h"
#include "speaker.h"
#include "video.h"
#include "pins.h"
#include "ui.h"
#include "usb_link.h"

static void set_mic(bool on) {
  if (on == mic_is_on()) return;
  mic_set_on(on);
  ui_set_mic(on);
  // 켜고 끈 것을 귀로도 알 수 있게 짧은 소리를 낸다 (블루투스 스피커가 있으면 그쪽, 없으면 PC 스피커)
  if (settings_mic_sound() && !speaker_play(on ? "mic_on" : "mic_off")) {
    link_send_event(on ? "{\"sound\":\"mic_on\"}" : "{\"sound\":\"mic_off\"}");
  }
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
// 자동 회전이 켜져 있으면 버튼은 잠깐만 돌린다: 기기를 다른 방향으로 세우면 다시 기울기를 따른다 (저장하지 않음).
//   (전에는 버튼 횟수를 저장해서, 한 번 어긋나면 세울 때마다 계속 틀린 방향으로 돌았다)
// 자동 회전을 끄면 버튼으로 맞춘 방향을 저장해 두고 그대로 쓴다.
static Preferences prefs;
static uint8_t rotate_offset = 0;
static int tilt = 0;

static void apply_rotation() {
  uint8_t rotation = (tilt + rotate_offset) & 3;
  if (rotation == display_rotation()) return;
  display_set_rotation(rotation);
  ui_layout();
  camera_ui_layout();
  saver_layout();
  Serial.printf("[main] screen rotation %d (tilt %d + button %d)\n", rotation, tilt, rotate_offset);
}

// 버튼으로 돌린 뒤 기울기가 이만큼 그대로면 기울기 방향으로 되돌린다 (설정 > 디스플레이 > 회전 되돌리기)
#define ROTATE_RESTORE_MS (5 * 60 * 1000UL)
static uint32_t stable_since = 0;

static void rotate_by_button() {
  rotate_offset = (rotate_offset + 1) & 3;
  stable_since = millis();
  if (!settings_auto_rotate()) prefs.putUChar("rot_fixed", (tilt + rotate_offset) & 3);
  apply_rotation();
}

// 기기를 돌려 세우면 화면도 따라 돈다. 같은 방향이 1초쯤 유지되어야 바꾼다 (들고 옮길 때 흔들림에 반응하지 않도록).
// 눕혀 놓았거나 애매하게 기울어 있으면 마지막 방향을 유지한다.
static void follow_tilt() {
  static uint32_t checked_ms = 0;
  static int candidate = -1;
  static uint8_t count = 0;

  if (millis() - checked_ms < 150 || !settings_auto_rotate()) return;
  checked_ms = millis();
  int now = imu_rotation();
  // 버튼으로 돌려 둔 상태: 기기가 움직이지 않고 5분 동안 그대로면 기울기 방향(눕혀 두었으면 마지막으로 세웠던 방향)으로
  // 되돌린다. 들거나 흔들리거나 다른 방향으로 세우면 처음부터 다시 센다.
  if (rotate_offset && settings_rotate_restore()) {
    static float last_x = 0, last_y = 0, last_z = 0;
    float x, y, z;
    bool moved = true;
    if (imu_read(&x, &y, &z)) {
      moved = fabsf(x - last_x) + fabsf(y - last_y) + fabsf(z - last_z) > 0.15f;
      last_x = x, last_y = y, last_z = z;
    }
    if (moved || (now >= 0 && now != tilt)) {
      stable_since = millis();
    } else if (millis() - stable_since >= ROTATE_RESTORE_MS) {
      Serial.println("[main] rotation restored to tilt after 5 min steady");
      rotate_offset = 0;
      apply_rotation();
    }
  }
  if (now < 0 || now == tilt) {
    candidate = -1;
    return;
  }
  count = now == candidate ? count + 1 : 1;
  candidate = now;
  if (count >= 7) {
    tilt = now;
    rotate_offset = 0;  // 새로 세운 방향은 기울기 그대로 (버튼으로 돌린 것은 잊는다)
    candidate = -1;
    apply_rotation();
  }
}

// 배터리 절약: 배터리로만 쓰는 동안 만지지 않으면 1분 뒤 어둡게, 3분 뒤 화면을 끈다.
// 화면 보호기: 전원과 상관없이 정해 둔 시간(기본 5분) 동안 만지지 않으면 어두운 시계 화면으로 바꾼다.
// 터치·BOOT 버튼·알림이 있으면 다시 켠다. 카메라를 쓰는 중에는 둘 다 하지 않는다.
#define DIM_AFTER_MS 60000
#define OFF_AFTER_MS 180000
// 보호기 화면의 밝기. 검은 바탕이라 백라이트를 낮춰도 시계는 잘 보인다.
#define SAVER_BRIGHTNESS 20

static void power_save() {
  static uint8_t shown = 255;
  static bool shown_saver = false;
  uint32_t idle = millis() - display_last_touch_ms();
  bool busy = camera_ui_open() || video_recording();
  bool battery_only = settings_auto_dim() && battery_pct() >= 0 && !battery_external_power() && !busy;
  bool saver = !busy && ((settings_saver_ms() && idle > settings_saver_ms()) || saver_demo_active());

  uint8_t level = settings_brightness();
  if (battery_only && idle > OFF_AFTER_MS) {
    level = 0;
  } else if (battery_only && idle > DIM_AFTER_MS) {
    level = 10;
  }
  if (saver && level > 0) level = min(level, (uint8_t)SAVER_BRIGHTNESS);
  if (level == shown && saver == shown_saver) return;
  bool first = shown == 255;
  shown = level;
  shown_saver = saver;
  saver_set_visible(saver);
  display_set_brightness(level);
  display_set_sleeping(saver || level < settings_brightness());
  if (!first) Serial.printf("[power] screen %s%s\n", level == 0 ? "off" : level < settings_brightness() ? "dim" : "on",
                            saver ? " (saver)" : "");
}

// 상태줄의 날짜와 시각. 시각은 Wi-Fi(인터넷) 또는 PC 도우미에게서 받으며, 받기 전에는 비워 둔다.
// 정보 화면 내용 (보이는 동안 1초마다)
static void show_info() {
  static uint32_t checked_ms = 0;
  static bool was_visible = false;
  bool visible = ui_info_visible();
  // 화면을 연 직후에는 바로 채운다
  bool just_opened = visible && !was_visible;
  was_visible = visible;
  if (!visible || (!just_opened && millis() - checked_ms < 1000)) return;
  checked_ms = millis();
  static const char *LINKS[] = {"없음", "USB", "블루투스", "Wi-Fi", "클라우드"};
  char text[480];
  char err[64];
  if (!health_last_error(err, sizeof(err))) strlcpy(err, "없음", sizeof(err));
  int pct = battery_pct();
  char bat[80];
  if (pct < 0) {
    snprintf(bat, sizeof(bat), "없음");
  } else {
    snprintf(bat, sizeof(bat), "%d%% (%.2fV)%s", pct, battery_mv() / 1000.0f, battery_charging() ? ", 충전 중" : "");
    int left = battery_minutes_left();
    if (left >= 0) {
      size_t n = strlen(bat);
      snprintf(bat + n, sizeof(bat) - n, ", 약 %d시간 %d분 남음", left / 60, left % 60);
    }
  }
  snprintf(text, sizeof(text),
           "PC 연결: %s\n"
           "Wi-Fi: %s\n"
           "배터리: %s\n"
           "화면 밝기: %d%%\n"
           "마이크: %s\n"
           "메모리: 내부 %uKB, PSRAM %uKB 남음\n"
           "켜진 지: %lu분 (%s)\n"
           "오류 재시작: %s\n"
           "펌웨어: " __DATE__ " " __TIME__,
           LINKS[link_active()], WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "연결 안 됨", bat,
           settings_brightness(), mic_is_on() ? "켜짐" : "꺼짐", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), (unsigned long)(millis() / 60000), health_boot_reason(), err);
  ui_set_info(text);
}

static void show_clock() {
  static uint32_t checked_ms = 0;
  if (millis() - checked_ms < 1000) return;
  checked_ms = millis();

  char date[24] = "", hm[8] = "";
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm local;
    localtime_r(&now, &local);
    snprintf(date, sizeof(date), "%d월 %d일", local.tm_mon + 1, local.tm_mday);
    snprintf(hm, sizeof(hm), "%02d:%02d", local.tm_hour, local.tm_min);
  }
  ui_set_clock(date, hm);
  ui_set_battery(battery_pct(), battery_charging());
}

// 내부 RAM 이 얼마나 남았는지 (부팅 단계별로 무엇이 얼마나 쓰는지 볼 때)
static void mem_log(const char *tag) {
  Serial.printf("[mem] %-10s internal free %6u largest %6u min %6u | psram free %u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void setup() {
  usb_link_init();
  Serial.begin(115200);
  pinMode(BTN_BOOT, INPUT_PULLUP);
  health_init();
  mem_log("start");

  display_init();
  mem_log("display");
  ui_init(toggle_mic, rotate_by_button);
  mem_log("ui");
  camera_init();
  mem_log("camera");
  camera_ui_init();
  mem_log("camera_ui");
  saver_init();
  ui_set_home_actions(camera_ui_show);
  imu_init();
  battery_init();
  prefs.begin("llm-deck");
  prefs.remove("rot_offset");  // 예전 펌웨어가 저장한 버튼 횟수 (어긋난 채 남아 있으면 계속 틀린 방향이 된다)
  settings_init();
  settings_set_mic_handler(set_mic);
  history_init();
  delay(50);  // 센서가 첫 값을 낼 때까지 잠깐 기다린다
  int start = imu_rotation();
  if (start >= 0) tilt = start;
  if (!settings_auto_rotate() && prefs.isKey("rot_fixed")) {
    // 자동 회전을 끈 채 버튼으로 맞춰 둔 방향
    tilt = prefs.getUChar("rot_fixed", 0) & 3;
    rotate_offset = 0;
  }
  float ax = 0, ay = 0, az = 0;
  imu_read(&ax, &ay, &az);
  Serial.printf("[main] tilt %d (accel %.2f %.2f %.2f), button %d\n", start, ax, ay, az, rotate_offset);
  apply_rotation();
  link_init(set_mic);
  mem_log("link");
  link_set_rotate(rotate_by_button);
  mic_init();
  mem_log("mic");
  net_init();
  mem_log("net");
  ble_link_init();
  mem_log("ble");
  cloud_init();
  mem_log("cloud");
  photo_init();
  mem_log("photo");
  video_init();
  mem_log("video");
  speaker_init();
  health_watch_start();
  Serial.println("[main] setup done");
}

void loop() {
  health_watch_feed();
  // 연결이 다 붙은 뒤(30초)와 그 뒤 5분마다 남은 내부 RAM 을 적는다
  static uint32_t mem_ms = 0;
  if ((!mem_ms && millis() > 30000) || (mem_ms && millis() - mem_ms > 300000)) {
    mem_ms = millis();
    mem_log("running");
  }
  static uint32_t level_ms = 0;

  display_loop();
  usb_link_loop();
  net_loop();
  link_loop();
  if (boot_button_pressed()) {
    display_poke();
    toggle_mic();
  }
  follow_tilt();
  battery_loop();
  show_clock();
  camera_ui_loop();
  sd_video_loop();
  health_loop();
  history_loop();
  pcpanel_loop();
  askpanel_loop();
  speaker_loop(settings_speaker_page());
  show_info();
  if (ui_settings_visible()) settings_loop();
  power_save();
  saver_loop();

  if (mic_is_on() && millis() - level_ms > 50) {
    level_ms = millis();
    ui_set_level(mic_level());
  }
  delay(5);
}
