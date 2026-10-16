#include "settings.h"

#include <Preferences.h>

#include "camera.h"
#include "ble_link.h"
#include "display.h"
#include "link.h"
#include "net.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define COLOR_DIM lv_color_hex(0x8A939E)
#define COLOR_ROW lv_color_hex(0x1C232B)
#define COLOR_ACCENT lv_color_hex(0xD97757)

static Preferences prefs;
static uint8_t brightness = 80;
static bool auto_rotate = true;
static bool mic_sound = true;
static bool auto_dim = true;
static bool limit_alerts = true;
// 화면 보호기가 켜지기까지 분 (0 이면 끔)
static const uint8_t SAVER_CHOICES[] = {0, 1, 5, 10, 30};
static uint8_t saver_min = 5;
static lv_obj_t *saver_lbl;
static lv_obj_t *dim_sw;
static lv_obj_t *alert_sw;

// 연결 방식별 허용 여부. 순서는 화면에 보이는 순서.
struct LinkOption {
  Link link;
  const char *key;
  const char *title;
  const char *desc;
  bool allowed;
  lv_obj_t *sw;
};
static LinkOption link_options[] = {
    {LINK_USB, "lk_usb", "USB", "케이블로 연결한 PC", true, nullptr},
    {LINK_BLE, "lk_ble", "블루투스", "끄면 검색 안 됨", true, nullptr},
    {LINK_WIFI, "lk_wifi", "Wi-Fi", "같은 공유기의 PC", true, nullptr},
    {LINK_CLOUD, "lk_cloud", "클라우드", "PC 없을 때 인터넷", true, nullptr},
};

static lv_obj_t *bright_slider;
static lv_obj_t *bright_value;
static lv_obj_t *rotate_sw;
static lv_obj_t *sound_sw;
static lv_obj_t *mirror_sw;
static lv_obj_t *cam_value;
static lv_obj_t *restart_lbl;
static uint32_t restart_armed_ms = 0;

void settings_init() {
  prefs.begin("llm-deck");
  brightness = prefs.getUChar("bright", 80);
  auto_rotate = prefs.getBool("auto_rot", true);
  mic_sound = prefs.getBool("mic_sound", true);
  auto_dim = prefs.getBool("auto_dim", true);
  limit_alerts = prefs.getBool("lim_alert", true);
  saver_min = prefs.getUChar("saver_min", 5);
  for (LinkOption &o : link_options) o.allowed = prefs.getBool(o.key, true);
  display_set_brightness(brightness);
}

bool settings_link_allowed(int link) {
  for (const LinkOption &o : link_options) {
    if (o.link == link) return o.allowed;
  }
  return true;
}

// 블루투스·Wi-Fi 는 끄면 무선도 실제로 끈다 (다른 기기가 찾거나 붙지 못하게)
static void apply_links() {
  ble_link_set_enabled(settings_link_allowed(LINK_BLE));
  net_set_enabled(settings_link_allowed(LINK_WIFI) || settings_link_allowed(LINK_CLOUD));
}

void settings_set_link_allowed(int link, bool allowed) {
  for (LinkOption &o : link_options) {
    if (o.link != link) continue;
    o.allowed = allowed;
    prefs.putBool(o.key, allowed);
    if (o.sw) {
      if (allowed) {
        lv_obj_add_state(o.sw, LV_STATE_CHECKED);
      } else {
        lv_obj_clear_state(o.sw, LV_STATE_CHECKED);
      }
    }
  }
  apply_links();
  Serial.printf("[settings] link %d %s\n", link, allowed ? "on" : "off");
}

static void link_changed(lv_event_t *e) {
  LinkOption *o = (LinkOption *)lv_event_get_user_data(e);
  o->allowed = lv_obj_has_state(o->sw, LV_STATE_CHECKED);
  prefs.putBool(o->key, o->allowed);
  apply_links();
}

uint8_t settings_brightness() {
  return brightness;
}

void settings_set_brightness(uint8_t pct) {
  brightness = constrain(pct, 10, 100);
  display_set_brightness(brightness);
  prefs.putUChar("bright", brightness);
}

void settings_cycle_brightness() {
  settings_set_brightness(brightness > 60 ? 60 : brightness > 30 ? 30 : 100);
}

bool settings_auto_rotate() {
  return auto_rotate;
}

bool settings_mic_sound() {
  return mic_sound;
}

bool settings_auto_dim() {
  return auto_dim;
}

bool settings_limit_alerts() {
  return limit_alerts;
}

uint32_t settings_saver_ms() {
  return saver_min * 60000UL;
}

static void show_saver() {
  if (saver_min) {
    lv_label_set_text_fmt(saver_lbl, "%d분", saver_min);
  } else {
    lv_label_set_text(saver_lbl, "끔");
  }
}

static void saver_clicked(lv_event_t *e) {
  // 누를 때마다 끔 -> 1분 -> 5분 -> 10분 -> 30분 -> 끔
  size_t i = 0;
  while (i < sizeof(SAVER_CHOICES) && SAVER_CHOICES[i] != saver_min) i++;
  saver_min = SAVER_CHOICES[(i + 1) % sizeof(SAVER_CHOICES)];
  prefs.putUChar("saver_min", saver_min);
  show_saver();
}

static void dim_changed(lv_event_t *e) {
  auto_dim = lv_obj_has_state(dim_sw, LV_STATE_CHECKED);
  prefs.putBool("auto_dim", auto_dim);
}

static void alert_changed(lv_event_t *e) {
  limit_alerts = lv_obj_has_state(alert_sw, LV_STATE_CHECKED);
  prefs.putBool("lim_alert", limit_alerts);
}

// 한 줄: 왼쪽에 이름(과 설명), 오른쪽에 조작 도구
static lv_obj_t *add_row(lv_obj_t *parent, const char *title, const char *desc) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  // 높이를 내용에 맞추면 오른쪽 가운데에 둔 스위치와 서로 크기를 기다리며 멈추므로 고정한다
  lv_obj_set_size(row, lv_pct(100), desc ? 62 : 44);
  lv_obj_set_style_bg_color(row, COLOR_ROW, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, 8, 0);
  lv_obj_set_style_pad_all(row, 8, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *t = lv_label_create(row);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_font(t, &font_ko_20, 0);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
  if (desc) {
    lv_obj_t *d = lv_label_create(row);
    lv_label_set_text(d, desc);
    lv_obj_set_style_text_font(d, &font_ko_14, 0);
    lv_obj_set_style_text_color(d, COLOR_DIM, 0);
    // 오른쪽 스위치·버튼과 겹치지 않게 폭을 줄이고, 넘치면 "..." 로 줄인다
    lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
    lv_obj_set_width(d, lv_pct(68));
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 0, 26);
  }
  return row;
}

static lv_obj_t *add_switch(lv_obj_t *row, bool on, lv_event_cb_t cb) {
  lv_obj_t *sw = lv_switch_create(row);
  lv_obj_set_size(sw, 50, 28);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return sw;
}

static lv_obj_t *add_button(lv_obj_t *row, const char *text, lv_event_cb_t cb, lv_obj_t **label) {
  lv_obj_t *btn = lv_btn_create(row);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_pad_hor(btn, 10, 0);
  lv_obj_set_style_pad_ver(btn, 6, 0);
  lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(btn);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &font_ko_14, 0);
  lv_obj_center(l);
  if (label) *label = l;
  return btn;
}

static void bright_changed(lv_event_t *e) {
  int v = lv_slider_get_value(bright_slider);
  lv_label_set_text_fmt(bright_value, "%d%%", v);
  display_set_brightness(v);
  // 손을 뗐을 때만 저장한다 (끄는 동안 계속 쓰지 않도록)
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_set_brightness(v);
}

static void rotate_changed(lv_event_t *e) {
  auto_rotate = lv_obj_has_state(rotate_sw, LV_STATE_CHECKED);
  prefs.putBool("auto_rot", auto_rotate);
}

static void sound_changed(lv_event_t *e) {
  mic_sound = lv_obj_has_state(sound_sw, LV_STATE_CHECKED);
  prefs.putBool("mic_sound", mic_sound);
}

static void show_camera_mount() {
  lv_label_set_text_fmt(cam_value, "%d°", camera_mount_rot() * 90);
}

static void cam_turn_clicked(lv_event_t *e) {
  camera_set_mount(camera_mount_rot() + 1, camera_mount_mirror());
  show_camera_mount();
}

static void mirror_changed(lv_event_t *e) {
  camera_set_mount(camera_mount_rot(), lv_obj_has_state(mirror_sw, LV_STATE_CHECKED));
}

static void restart_clicked(lv_event_t *e) {
  // 실수로 누르지 않게 3초 안에 한 번 더 눌러야 재시작한다
  if (restart_armed_ms && millis() - restart_armed_ms < 3000) {
    Serial.println("[settings] restart");
    delay(100);
    ESP.restart();
  }
  restart_armed_ms = millis();
  lv_label_set_text(restart_lbl, "한 번 더 누르기");
}

void settings_build(lv_obj_t *parent) {
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(parent, 6, 0);
  lv_obj_set_scroll_dir(parent, LV_DIR_VER);

  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, "설정");
  lv_obj_set_style_text_font(title, &font_ko_20, 0);
  lv_obj_set_style_text_color(title, COLOR_ACCENT, 0);

  lv_obj_t *row = add_row(parent, "화면 밝기", nullptr);
  bright_value = lv_label_create(row);
  lv_obj_set_style_text_font(bright_value, &font_ko_14, 0);
  lv_obj_set_style_text_color(bright_value, COLOR_DIM, 0);
  lv_obj_align(bright_value, LV_ALIGN_TOP_RIGHT, 0, 4);
  bright_slider = lv_slider_create(row);
  lv_obj_set_width(bright_slider, lv_pct(92));
  lv_obj_align(bright_slider, LV_ALIGN_TOP_MID, 0, 36);
  lv_slider_set_range(bright_slider, 10, 100);
  // 밀어서 밝기를 바꿀 때 "오른쪽으로 밀면 홈" 이 같이 일어나지 않게 한다
  lv_obj_clear_flag(bright_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(bright_slider, bright_changed, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(bright_slider, bright_changed, LV_EVENT_RELEASED, NULL);
  lv_obj_set_height(row, 64);

  add_button(add_row(parent, "화면 보호기", "안 쓰면 어두운 시계로"), "5분", saver_clicked, &saver_lbl);
  dim_sw = add_switch(add_row(parent, "배터리 절약", "1분 어둡게, 3분 끄기"), auto_dim, dim_changed);
  alert_sw = add_switch(add_row(parent, "한도 알림", "80%, 90%, 초기화 때"), limit_alerts, alert_changed);
  rotate_sw = add_switch(add_row(parent, "자동 회전", "기울이면 따라 돌기"), auto_rotate, rotate_changed);
  sound_sw = add_switch(add_row(parent, "마이크 알림음", "켜고 끌 때 PC 소리"), mic_sound, sound_changed);

  row = add_row(parent, "카메라 방향", "사진이 누우면");
  add_button(row, "돌리기", cam_turn_clicked, nullptr);
  cam_value = lv_label_create(row);
  lv_obj_set_style_text_font(cam_value, &font_ko_14, 0);
  lv_obj_set_style_text_color(cam_value, COLOR_DIM, 0);
  lv_obj_align(cam_value, LV_ALIGN_RIGHT_MID, -76, 0);

  mirror_sw = add_switch(add_row(parent, "카메라 좌우 반전", "글자가 거꾸로면"),
                         camera_mount_mirror(), mirror_changed);

  lv_obj_t *section = lv_label_create(parent);
  lv_label_set_text(section, "연결 방식");
  lv_obj_set_style_text_font(section, &font_ko_14, 0);
  lv_obj_set_style_text_color(section, COLOR_DIM, 0);
  lv_obj_set_style_pad_top(section, 6, 0);
  for (LinkOption &o : link_options) {
    o.sw = lv_switch_create(add_row(parent, o.title, o.desc));
    lv_obj_set_size(o.sw, 50, 28);
    lv_obj_align(o.sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(o.sw, link_changed, LV_EVENT_VALUE_CHANGED, &o);
  }

  add_button(add_row(parent, "기기 재시작", nullptr), "재시작", restart_clicked, &restart_lbl);

  settings_refresh();
}

void settings_refresh() {
  lv_slider_set_value(bright_slider, brightness, LV_ANIM_OFF);
  lv_label_set_text_fmt(bright_value, "%d%%", brightness);
  if (auto_rotate) {
    lv_obj_add_state(rotate_sw, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(rotate_sw, LV_STATE_CHECKED);
  }
  if (auto_dim) {
    lv_obj_add_state(dim_sw, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(dim_sw, LV_STATE_CHECKED);
  }
  if (limit_alerts) {
    lv_obj_add_state(alert_sw, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(alert_sw, LV_STATE_CHECKED);
  }
  if (mic_sound) {
    lv_obj_add_state(sound_sw, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(sound_sw, LV_STATE_CHECKED);
  }
  if (camera_mount_mirror()) {
    lv_obj_add_state(mirror_sw, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(mirror_sw, LV_STATE_CHECKED);
  }
  for (LinkOption &o : link_options) {
    if (o.allowed) {
      lv_obj_add_state(o.sw, LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(o.sw, LV_STATE_CHECKED);
    }
  }
  show_camera_mount();
  show_saver();
  restart_armed_ms = 0;
  lv_label_set_text(restart_lbl, "재시작");
}
