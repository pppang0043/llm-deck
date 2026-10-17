#include "settings.h"

#include <Preferences.h>
#include <WiFi.h>

#include "battery.h"
#include "ble_link.h"
#include "camera.h"
#include "display.h"
#include "link.h"
#include "net.h"
#include "speaker.h"
#include "speaker_proto.h"
#include "storage.h"
#include "ui.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define COLOR_DIM lv_color_hex(0x8A939E)
#define COLOR_ROW lv_color_hex(0x1C232B)
#define COLOR_LINE lv_color_hex(0x2A323B)
#define COLOR_ACCENT lv_color_hex(0xD97757)
#define COLOR_DANGER lv_color_hex(0xE0483C)

// ---- 저장되는 설정 값 ----
static Preferences prefs;
static uint8_t brightness = 80;
static bool auto_rotate = true;
static bool mic_sound = true;
static bool auto_dim = true;
static bool limit_alerts = true;
static bool sd_photos = true;
static bool sd_videos = true;
// 화면 보호기가 켜지기까지 분 (0 이면 끔)
static const uint8_t SAVER_CHOICES[] = {0, 1, 5, 10, 30};
static uint8_t saver_min = 5;
// 블루투스 스피커 (ATOM Lite 모듈)
static bool spk_on = true;
static bool spk_keep = false;
static uint8_t spk_vol = 60;
static const uint8_t SPK_VOL_CHOICES[] = {20, 40, 60, 80, 100};

// 연결 방식별 허용 여부
struct LinkOption {
  Link link;
  const char *key;
  bool allowed;
  lv_obj_t *sw;
};
static LinkOption link_options[] = {
    {LINK_USB, "lk_usb", true, nullptr},
    {LINK_BLE, "lk_ble", true, nullptr},
    {LINK_WIFI, "lk_wifi", true, nullptr},
    {LINK_CLOUD, "lk_cloud", true, nullptr},
};

// ---- 화면 구성 ----
// 아이폰 설정처럼 첫 화면은 분류 목록이고, 누르면 그 분류의 화면으로 들어간다.
// 들어간 화면에서 오른쪽으로 밀거나 "< 설정" 을 누르면 목록으로 돌아온다.
enum Page { PAGE_LIST, PAGE_WIFI, PAGE_BLE, PAGE_SPEAKER, PAGE_NOTIFY, PAGE_DISPLAY, PAGE_BATTERY, PAGE_CAMERA, PAGE_SD,
            PAGE_GENERAL, PAGE_COUNT };
static const char *PAGE_KEYS[] = {"list", "wifi", "bluetooth", "speaker", "notify", "display", "battery", "camera", "sd", "general"};
static lv_obj_t *pages[PAGE_COUNT];
static Page current = PAGE_LIST;

// 목록 오른쪽에 보이는 현재 상태 글자
static lv_obj_t *wifi_value, *ble_value, *spk_value, *battery_value, *sd_value;

static lv_obj_t *bright_slider, *bright_value;
static lv_obj_t *rotate_sw, *sound_sw, *mirror_sw, *dim_sw, *alert_sw, *sd_sw, *sd_video_sw;
static lv_obj_t *saver_lbl, *cam_value;
static lv_obj_t *wifi_status, *wifi_ip, *wifi_signal, *wifi_list;
static lv_obj_t *ble_status, *ble_none;
// 연결된 PC 한 대씩의 줄 (자리 수만큼 만들어 두고, 빈 자리는 숨긴다)
static lv_obj_t *ble_rows[BLE_MAX_PEERS], *ble_names[BLE_MAX_PEERS], *ble_states[BLE_MAX_PEERS];
static lv_obj_t *bat_level, *bat_state;
static lv_obj_t *sd_status, *sd_space, *sd_count, *sd_vcount;
static lv_obj_t *uptime_value;
static lv_obj_t *spk_status, *spk_target, *spk_on_sw, *spk_keep_sw, *spk_vol_lbl, *spk_none, *spk_find_lbl;
static lv_obj_t *spk_rows[SPK_MAX_FOUND], *spk_names[SPK_MAX_FOUND], *spk_signals[SPK_MAX_FOUND];

// 실수로 누르지 않게 3초 안에 한 번 더 눌러야 하는 버튼
struct Confirm {
  lv_obj_t *label;
  const char *text;
  uint32_t armed_ms;
};
static Confirm restart_btn = {nullptr, "재시작", 0};
static Confirm wifi_clear_btn = {nullptr, "지우기", 0};
static Confirm sd_clear_btn = {nullptr, "지우기", 0};
static Confirm sd_format_btn = {nullptr, "포맷", 0};
static Confirm spk_forget_btn = {nullptr, "잊기", 0};
static lv_obj_t *sd_format_row;

static bool confirmed(Confirm &c) {
  if (c.armed_ms && millis() - c.armed_ms < 3000) {
    c.armed_ms = 0;
    lv_label_set_text(c.label, c.text);
    return true;
  }
  c.armed_ms = millis();
  lv_label_set_text(c.label, "한 번 더");
  return false;
}

static void disarm(Confirm &c) {
  c.armed_ms = 0;
  if (c.label) lv_label_set_text(c.label, c.text);
}

static void set_checked(lv_obj_t *obj, bool on) {
  if (!obj) return;
  if (on) {
    lv_obj_add_state(obj, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(obj, LV_STATE_CHECKED);
  }
}

// ---- 값 읽기 ----

void settings_init() {
  prefs.begin("llm-deck");
  brightness = prefs.getUChar("bright", 80);
  auto_rotate = prefs.getBool("auto_rot", true);
  mic_sound = prefs.getBool("mic_sound", true);
  auto_dim = prefs.getBool("auto_dim", true);
  limit_alerts = prefs.getBool("lim_alert", true);
  sd_photos = prefs.getBool("sd_photos", true);
  sd_videos = prefs.getBool("sd_videos", true);
  saver_min = prefs.getUChar("saver_min", 5);
  spk_on = prefs.getBool("spk_on", true);
  spk_keep = prefs.getBool("spk_keep", false);
  spk_vol = prefs.getUChar("spk_vol", 60);
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
    set_checked(o.sw, allowed);
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

bool settings_sd_photos() {
  return sd_photos;
}

bool settings_sd_videos() {
  return sd_videos;
}

uint32_t settings_saver_ms() {
  return saver_min * 60000UL;
}

bool settings_speaker_on() {
  return spk_on;
}

bool settings_speaker_keep() {
  return spk_keep;
}

uint8_t settings_speaker_volume() {
  return spk_vol;
}

bool settings_speaker_page() {
  return ui_settings_visible() && current == PAGE_SPEAKER;
}

// ---- 화면 부품 ----

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, color, 0);
  return lbl;
}

// 세로로 쌓이고 위아래로 넘기는 한 화면
static lv_obj_t *add_page(lv_obj_t *parent) {
  lv_obj_t *page = lv_obj_create(parent);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, lv_pct(100), lv_pct(100));
  lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(page, 6, 0);
  lv_obj_set_style_pad_bottom(page, 10, 0);
  lv_obj_set_scroll_dir(page, LV_DIR_VER);
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  return page;
}

// 분류 사이의 작은 제목
static void add_section(lv_obj_t *page, const char *text) {
  lv_obj_t *s = add_label(page, text, &font_ko_14, COLOR_DIM);
  lv_obj_set_style_pad_top(s, 6, 0);
}

// 회색 설명 글 (여러 줄)
static lv_obj_t *add_note_label(lv_obj_t *page, const char *text) {
  lv_obj_t *n = add_label(page, text, &font_ko_14, COLOR_DIM);
  lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(n, lv_pct(100));
  return n;
}

static void add_note(lv_obj_t *page, const char *text) {
  add_note_label(page, text);
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
  lv_obj_t *t = add_label(row, title, &font_ko_20, lv_color_white());
  lv_obj_align(t, desc ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 0, 0);
  if (desc) {
    lv_obj_t *d = add_label(row, desc, &font_ko_14, COLOR_DIM);
    // 오른쪽 스위치·버튼과 겹치지 않게 폭을 줄이고, 넘치면 "..." 로 줄인다
    lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
    lv_obj_set_width(d, lv_pct(68));
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 0, 26);
  }
  return row;
}

// 이름과 값만 있는 줄. 값 글자를 돌려준다.
static lv_obj_t *add_info(lv_obj_t *parent, const char *title) {
  lv_obj_t *row = add_row(parent, title, nullptr);
  lv_obj_t *v = add_label(row, "", &font_ko_14, COLOR_DIM);
  lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_width(v, lv_pct(62));
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
  return v;
}

static lv_obj_t *add_switch(lv_obj_t *row, bool on, lv_event_cb_t cb, void *user = nullptr) {
  lv_obj_t *sw = lv_switch_create(row);
  lv_obj_set_size(sw, 50, 28);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  set_checked(sw, on);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user);
  return sw;
}

static lv_obj_t *add_button(lv_obj_t *row, const char *text, lv_event_cb_t cb, lv_obj_t **label,
                            bool danger = false) {
  lv_obj_t *btn = lv_btn_create(row);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_pad_hor(btn, 10, 0);
  lv_obj_set_style_pad_ver(btn, 6, 0);
  if (danger) lv_obj_set_style_bg_color(btn, COLOR_DANGER, 0);
  lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = add_label(btn, text, &font_ko_14, lv_color_white());
  lv_obj_center(l);
  if (label) *label = l;
  return btn;
}

static lv_obj_t *add_link_switch(lv_obj_t *row, Link link) {
  for (LinkOption &o : link_options) {
    if (o.link != link) continue;
    o.sw = add_switch(row, o.allowed, link_changed, &o);
    return o.sw;
  }
  return nullptr;
}

// ---- 화면 이동 ----

static void refresh_page();

static void show_page(Page page) {
  for (int i = 0; i < PAGE_COUNT; i++) {
    if (i == page) {
      lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  current = page;
  lv_obj_scroll_to_y(pages[page], 0, LV_ANIM_OFF);
  disarm(restart_btn);
  disarm(wifi_clear_btn);
  disarm(sd_clear_btn);
  disarm(sd_format_btn);
  disarm(spk_forget_btn);
  refresh_page();
}

static void back_clicked(lv_event_t *e) {
  show_page(PAGE_LIST);
}

static void menu_clicked(lv_event_t *e) {
  show_page((Page)(intptr_t)lv_event_get_user_data(e));
}

// 들어간 화면의 맨 위: "< 설정" 과 화면 이름
static lv_obj_t *add_sub_page(lv_obj_t *parent, Page page, const char *title) {
  lv_obj_t *p = add_page(parent);
  pages[page] = p;
  lv_obj_t *head = lv_obj_create(p);
  lv_obj_remove_style_all(head);
  lv_obj_set_size(head, lv_pct(100), 30);
  lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *back = add_label(head, LV_SYMBOL_LEFT " 설정", &font_ko_14, COLOR_ACCENT);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(back, 12);
  lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
  lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_align(add_label(head, title, &font_ko_20, lv_color_white()), LV_ALIGN_CENTER, 0, 0);
  return p;
}

// 목록의 한 묶음 (둥근 상자 안에 여러 줄)
static lv_obj_t *add_group(lv_obj_t *page) {
  lv_obj_t *g = lv_obj_create(page);
  lv_obj_remove_style_all(g);
  lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(g, COLOR_ROW, 0);
  lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g, 10, 0);
  lv_obj_set_style_clip_corner(g, true, 0);
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  return g;
}

// 목록의 한 줄: [색 아이콘] 이름 ......... 값 >
// page 가 PAGE_LIST 면 누를 수 없는 줄(오른쪽에 스위치를 직접 둔다)
static lv_obj_t *add_menu(lv_obj_t *group, const char *symbol, uint32_t color, const char *title, Page page,
                          lv_obj_t **value, int value_pct = 40) {
  lv_obj_t *row = lv_obj_create(group);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 44);
  lv_obj_set_style_pad_hor(row, 8, 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_color(row, COLOR_LINE, 0);
  lv_obj_set_style_border_width(row, lv_obj_get_child_cnt(group) > 1 ? 1 : 0, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  if (page != PAGE_LIST) {
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, COLOR_LINE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, menu_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)page);
  }

  lv_obj_t *icon = lv_obj_create(row);
  lv_obj_remove_style_all(icon);
  lv_obj_set_size(icon, 28, 28);
  lv_obj_set_style_radius(icon, 7, 0);
  lv_obj_set_style_bg_color(icon, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_center(add_label(icon, symbol, &lv_font_montserrat_14, lv_color_white()));

  lv_obj_t *t = add_label(row, title, &font_ko_20, lv_color_white());
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 38, 0);

  if (page != PAGE_LIST) {
    lv_obj_t *chev = add_label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, COLOR_DIM);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);
    if (value) {
      *value = add_label(row, "", &font_ko_14, COLOR_DIM);
      lv_label_set_long_mode(*value, LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_RIGHT, 0);
      // 세로 화면(폭 240)에서도 왼쪽 이름과 겹치지 않는 폭. 넘치면 "..." 로 줄인다.
      lv_obj_set_width(*value, lv_pct(value_pct));
      lv_obj_align(*value, LV_ALIGN_RIGHT_MID, -18, 0);
    }
  }
  return row;
}

// ---- 조작 ----

static void dim_changed(lv_event_t *e) {
  auto_dim = lv_obj_has_state(dim_sw, LV_STATE_CHECKED);
  prefs.putBool("auto_dim", auto_dim);
}

static void alert_changed(lv_event_t *e) {
  limit_alerts = lv_obj_has_state(alert_sw, LV_STATE_CHECKED);
  prefs.putBool("lim_alert", limit_alerts);
}

static void rotate_changed(lv_event_t *e) {
  auto_rotate = lv_obj_has_state(rotate_sw, LV_STATE_CHECKED);
  prefs.putBool("auto_rot", auto_rotate);
}

static void sound_changed(lv_event_t *e) {
  mic_sound = lv_obj_has_state(sound_sw, LV_STATE_CHECKED);
  prefs.putBool("mic_sound", mic_sound);
}

static void sd_changed(lv_event_t *e) {
  sd_photos = lv_obj_has_state(sd_sw, LV_STATE_CHECKED);
  prefs.putBool("sd_photos", sd_photos);
}

static void sd_video_changed(lv_event_t *e) {
  sd_videos = lv_obj_has_state(sd_video_sw, LV_STATE_CHECKED);
  prefs.putBool("sd_videos", sd_videos);
}

static void bright_changed(lv_event_t *e) {
  int v = lv_slider_get_value(bright_slider);
  lv_label_set_text_fmt(bright_value, "%d%%", v);
  display_set_brightness(v);
  // 손을 뗐을 때만 저장한다 (끄는 동안 계속 쓰지 않도록)
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_set_brightness(v);
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
  if (!confirmed(restart_btn)) return;
  Serial.println("[settings] restart");
  delay(100);
  ESP.restart();
}

static void ble_row_clicked(lv_event_t *e) {
  int slot = (intptr_t)lv_event_get_user_data(e);
  if (ble_peer_get(slot, nullptr, 0) == 1) {
    ble_peer_select(slot);
    ui_toast("이 PC를 사용합니다\n마이크, 사진, 알림이 이 PC로 갑니다", 2500);
  }
}

static void ble_disconnect_clicked(lv_event_t *e) {
  ble_peer_disconnect((intptr_t)lv_event_get_user_data(e));
  ui_toast("블루투스 연결을 끊었습니다", 2000);
}

static void spk_on_changed(lv_event_t *e) {
  spk_on = lv_obj_has_state(spk_on_sw, LV_STATE_CHECKED);
  prefs.putBool("spk_on", spk_on);
}

static void spk_keep_changed(lv_event_t *e) {
  spk_keep = lv_obj_has_state(spk_keep_sw, LV_STATE_CHECKED);
  prefs.putBool("spk_keep", spk_keep);
}

static void show_spk_vol() {
  lv_label_set_text_fmt(spk_vol_lbl, "%d%%", spk_vol);
}

static void spk_vol_clicked(lv_event_t *e) {
  // 누를 때마다 20 -> 40 -> 60 -> 80 -> 100 -> 20%
  size_t i = 0;
  while (i < sizeof(SPK_VOL_CHOICES) && SPK_VOL_CHOICES[i] != spk_vol) i++;
  spk_vol = SPK_VOL_CHOICES[(i + 1) % sizeof(SPK_VOL_CHOICES)];
  prefs.putUChar("spk_vol", spk_vol);
  show_spk_vol();
}

static void spk_test_clicked(lv_event_t *e) {
  if (!speaker_present()) {
    ui_toast("스피커 모듈이 없습니다\n배선을 확인하세요", 2500);
  } else if (!speaker_target(nullptr, 0)) {
    ui_toast("먼저 스피커를 찾아 연결하세요", 2500);
  } else if (!spk_on) {
    ui_toast("\"스피커로 알림음\"을 켜세요", 2500);
  } else {
    speaker_play("test");
    ui_toast(speaker_state() >= SPK_STATE_CONNECTED ? "시험 소리를 냅니다" : "스피커에 연결한 뒤 소리를 냅니다\n(몇 초 걸림)",
             2500);
  }
}

static void spk_find_clicked(lv_event_t *e) {
  if (!speaker_present()) {
    ui_toast("스피커 모듈이 없습니다\n배선을 확인하세요", 2500);
    return;
  }
  speaker_find();
  ui_toast("스피커를 찾는 중... (1분)\n스피커를 페어링 모드로 두세요", 3000);
}

static void spk_row_clicked(lv_event_t *e) {
  int i = (intptr_t)lv_event_get_user_data(e);
  char name[SPK_NAME_LEN];
  if (!speaker_found(i, name, sizeof(name), nullptr)) return;
  speaker_connect(i);
  char text[64];
  snprintf(text, sizeof(text), "%s\n연결하는 중... 되면 소리가 납니다", name);
  ui_toast(text, 4000);
}

static void spk_forget_clicked(lv_event_t *e) {
  if (!confirmed(spk_forget_btn)) return;
  speaker_forget();
  ui_toast("기억한 스피커를 지웠습니다", 2000);
}

static void wifi_clear_clicked(lv_event_t *e) {
  if (!confirmed(wifi_clear_btn)) return;
  net_clear_wifi();
  refresh_page();
  ui_toast("추가한 Wi-Fi를 지웠습니다\n재시작하면 적용됩니다", 3000);
}

static void sd_recheck_clicked(lv_event_t *e) {
  bool ok = storage_recheck();
  refresh_page();
  ui_toast(ok ? "SD 카드를 찾았습니다" : "SD 카드가 없습니다", 2000);
}

static void sd_format_clicked(lv_event_t *e) {
  if (!confirmed(sd_format_btn)) return;
  ui_toast("SD 카드를 포맷하는 중...", 30000);
  lv_refr_now(NULL);
  bool ok = storage_format();
  refresh_page();
  ui_toast(ok ? "SD 카드를 FAT32로 포맷했습니다" : "포맷하지 못했습니다\n카드가 꽂혀 있는지 확인하세요", 3000);
}

static void sd_clear_clicked(lv_event_t *e) {
  if (!confirmed(sd_clear_btn)) return;
  int photos = storage_clear("/photos");
  int videos = storage_clear("/videos");
  refresh_page();
  char text[64];
  snprintf(text, sizeof(text), "사진 %d장, 동영상 %d개를\n지웠습니다", photos, videos);
  ui_toast(text, 2500);
}

// ---- 화면 만들기 ----

static void build_list(lv_obj_t *parent) {
  lv_obj_t *p = add_page(parent);
  pages[PAGE_LIST] = p;
  add_label(p, "설정", &font_ko_20, COLOR_ACCENT);

  lv_obj_t *g = add_group(p);
  add_menu(g, LV_SYMBOL_WIFI, 0x2F7DE1, "Wi-Fi", PAGE_WIFI, &wifi_value);
  add_menu(g, LV_SYMBOL_BLUETOOTH, 0x2F7DE1, "블루투스", PAGE_BLE, &ble_value, 30);
  add_menu(g, LV_SYMBOL_AUDIO, 0xE0483C, "스피커", PAGE_SPEAKER, &spk_value, 34);
  add_link_switch(add_menu(g, LV_SYMBOL_USB, 0x4CAF50, "USB", PAGE_LIST, nullptr), LINK_USB);

  g = add_group(p);
  add_menu(g, LV_SYMBOL_BELL, 0xE0483C, "알림 및 사운드", PAGE_NOTIFY, nullptr);
  add_menu(g, LV_SYMBOL_EYE_OPEN, 0x2F7DE1, "디스플레이", PAGE_DISPLAY, nullptr);
  add_menu(g, LV_SYMBOL_BATTERY_FULL, 0x4CAF50, "배터리", PAGE_BATTERY, &battery_value);
  add_menu(g, LV_SYMBOL_IMAGE, 0x6B7280, "카메라", PAGE_CAMERA, nullptr);

  g = add_group(p);
  add_menu(g, LV_SYMBOL_SD_CARD, 0x6B7280, "SD 카드", PAGE_SD, &sd_value, 34);
  add_menu(g, LV_SYMBOL_SETTINGS, 0x6B7280, "일반", PAGE_GENERAL, nullptr);
}

static void build_wifi(lv_obj_t *parent) {
  lv_obj_t *p = add_sub_page(parent, PAGE_WIFI, "Wi-Fi");
  add_link_switch(add_row(p, "Wi-Fi", "같은 공유기의 PC와 연결"), LINK_WIFI);
  wifi_status = add_info(p, "접속");
  wifi_ip = add_info(p, "IP 주소");
  wifi_signal = add_info(p, "신호");

  add_section(p, "등록된 Wi-Fi");
  lv_obj_t *box = lv_obj_create(p);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(box, COLOR_ROW, 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(box, 8, 0);
  lv_obj_set_style_pad_all(box, 8, 0);
  wifi_list = add_label(box, "", &font_ko_20, lv_color_white());
  add_note(p, "추가: USB로 연결한 PC에서\nuv run main.py --add-wifi (2.4GHz, 최대 5개)");
  add_button(add_row(p, "추가한 Wi-Fi 지우기", "기본 Wi-Fi는 남음"), "지우기", wifi_clear_clicked,
             &wifi_clear_btn.label, true);

  add_section(p, "클라우드");
  add_link_switch(add_row(p, "클라우드", "PC 없을 때 인터넷에서 받기"), LINK_CLOUD);
}

static void build_ble(lv_obj_t *parent) {
  lv_obj_t *p = add_sub_page(parent, PAGE_BLE, "블루투스");
  add_link_switch(add_row(p, "블루투스", "끄면 PC가 찾지 못함"), LINK_BLE);
  lv_label_set_text(add_info(p, "기기 이름"), BLE_NAME);
  ble_status = add_info(p, "상태");

  add_section(p, "연결된 PC (최대 3대)");
  ble_none = add_note_label(p, "연결된 PC가 없습니다.");
  for (int i = 0; i < BLE_MAX_PEERS; i++) {
    lv_obj_t *row = add_row(p, "", "");
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, COLOR_LINE, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, ble_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    ble_names[i] = lv_obj_get_child(row, 0);
    lv_label_set_long_mode(ble_names[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(ble_names[i], lv_pct(68));
    ble_states[i] = lv_obj_get_child(row, 1);
    lv_obj_t *btn = lv_btn_create(row);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 10, 0);
    lv_obj_set_style_pad_ver(btn, 6, 0);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(btn, ble_disconnect_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_center(add_label(btn, "끊기", &font_ko_14, lv_color_white()));
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    ble_rows[i] = row;
  }
  add_note(p, "기다리는 PC를 누르면 그 PC를 사용합니다. 마이크 소리, 사진, 알림음과 화면의 사용량은 사용 중인 PC 하나만 씁니다.");
  add_note(p, "PC 운영체제에서 페어링하지 마세요. PC 도우미가 자동으로 찾아 연결합니다.");
}

static void build_speaker(lv_obj_t *parent) {
  lv_obj_t *p = add_sub_page(parent, PAGE_SPEAKER, "스피커");
  spk_status = add_info(p, "상태");
  spk_target = add_info(p, "스피커");
  spk_on_sw = add_switch(add_row(p, "스피커로 알림음", "끄면 PC 스피커로"), spk_on, spk_on_changed);
  spk_keep_sw = add_switch(add_row(p, "항상 연결", "소리가 바로 남, 전기 더 씀"), spk_keep, spk_keep_changed);
  add_button(add_row(p, "소리 크기", nullptr), "60%", spk_vol_clicked, &spk_vol_lbl);
  add_button(add_row(p, "시험 소리", nullptr), "듣기", spk_test_clicked, nullptr);

  add_section(p, "새 스피커 연결");
  add_button(add_row(p, "주변 스피커 찾기", "스피커를 페어링 모드로"), "찾기", spk_find_clicked, &spk_find_lbl);
  spk_none = add_note_label(p, "");
  for (int i = 0; i < SPK_MAX_FOUND; i++) {
    lv_obj_t *row = add_row(p, "", "");
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, COLOR_LINE, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, spk_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    spk_names[i] = lv_obj_get_child(row, 0);
    lv_label_set_long_mode(spk_names[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(spk_names[i], lv_pct(100));
    spk_signals[i] = lv_obj_get_child(row, 1);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    spk_rows[i] = row;
  }
  add_button(add_row(p, "스피커 잊기", "다른 스피커를 쓸 때"), "잊기", spk_forget_clicked, &spk_forget_btn.label,
             true);
  add_note(p, "이 보드는 블루투스 스피커를 직접 연결하지 못해 작은 스피커 모듈(ATOM Lite)이 대신 연결합니다. 소리를 낼 때만 연결하고 30초 뒤 끊어 전기를 아낍니다. 처음 소리는 연결하느라 몇 초 늦게 납니다.");
}

static void build_notify(lv_obj_t *parent) {
  lv_obj_t *p = add_sub_page(parent, PAGE_NOTIFY, "알림 및 사운드");
  alert_sw = add_switch(add_row(p, "한도 알림", "80%, 90%, 초기화 때"), limit_alerts, alert_changed);
  sound_sw = add_switch(add_row(p, "마이크 알림음", "켜고 끌 때 소리"), mic_sound, sound_changed);
  add_note(p, "알림음은 블루투스 스피커를 연결해 두었으면 그 스피커로, 아니면 사용 중인 PC의 스피커로 나옵니다.");
}

static void build_display(lv_obj_t *parent) {
  lv_obj_t *p = add_sub_page(parent, PAGE_DISPLAY, "디스플레이");
  lv_obj_t *row = add_row(p, "밝기", nullptr);
  lv_obj_align(lv_obj_get_child(row, 0), LV_ALIGN_TOP_LEFT, 0, 0);
  bright_value = add_label(row, "", &font_ko_14, COLOR_DIM);
  lv_obj_align(bright_value, LV_ALIGN_TOP_RIGHT, 0, 4);
  bright_slider = lv_slider_create(row);
  lv_obj_set_width(bright_slider, lv_pct(92));
  lv_obj_align(bright_slider, LV_ALIGN_TOP_MID, 0, 36);
  lv_slider_set_range(bright_slider, 10, 100);
  // 밀어서 밝기를 바꿀 때 "오른쪽으로 밀면 뒤로" 가 같이 일어나지 않게 한다
  lv_obj_clear_flag(bright_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(bright_slider, bright_changed, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(bright_slider, bright_changed, LV_EVENT_RELEASED, NULL);
  lv_obj_set_height(row, 64);

  add_button(add_row(p, "화면 보호기", "안 쓰면 어두운 시계로"), "5분", saver_clicked, &saver_lbl);
  rotate_sw = add_switch(add_row(p, "자동 회전", "기울이면 따라 돌기"), auto_rotate, rotate_changed);
  add_note(p, "화면 보호기는 시계를 30초마다 옮기고 밝기를 낮춰 화면 잔상과 백라이트 닳음을 줄입니다.");
}

static void build_battery(lv_obj_t *parent) {
  lv_obj_t *p = add_sub_page(parent, PAGE_BATTERY, "배터리");
  bat_level = add_info(p, "잔량");
  bat_state = add_info(p, "전원");
  dim_sw = add_switch(add_row(p, "배터리 절약", "1분 어둡게, 3분 끄기"), auto_dim, dim_changed);
  add_note(p, "배터리 절약은 배터리로만 쓸 때 동작합니다. 잔량은 전압으로 짐작하며, 충전이 끝난 채 USB에 20분 꽂혀 있으면 스스로 보정합니다.");
}

static void build_camera(lv_obj_t *parent) {
  lv_obj_t *p = add_sub_page(parent, PAGE_CAMERA, "카메라");
  lv_obj_t *row = add_row(p, "방향", "사진이 누우면");
  add_button(row, "돌리기", cam_turn_clicked, nullptr);
  cam_value = add_label(row, "", &font_ko_14, COLOR_DIM);
  lv_obj_align(cam_value, LV_ALIGN_RIGHT_MID, -76, 0);
  mirror_sw = add_switch(add_row(p, "좌우 반전", "글자가 거꾸로면"), camera_mount_mirror(), mirror_changed);
  add_note(p, "지금 카메라는 0°, 좌우 반전 켬이 맞습니다.");
}

static void build_sd(lv_obj_t *parent) {
  lv_obj_t *p = add_sub_page(parent, PAGE_SD, "SD 카드");
  sd_status = add_info(p, "상태");
  sd_space = add_info(p, "사용량");
  sd_count = add_info(p, "사진");
  sd_vcount = add_info(p, "동영상");
  sd_sw = add_switch(add_row(p, "사진 저장", "찍으면 카드에도 저장"), sd_photos, sd_changed);
  sd_video_sw = add_switch(add_row(p, "동영상 저장", "녹화하면 카드에도 저장"), sd_videos, sd_video_changed);
  add_button(add_row(p, "다시 찾기", "카드를 방금 꽂았을 때"), "찾기", sd_recheck_clicked, nullptr);
  add_button(add_row(p, "모두 지우기", "카드의 사진과 동영상"), "지우기", sd_clear_clicked, &sd_clear_btn.label,
             true);
  sd_format_row = add_row(p, "카드 포맷", "읽을 수 없을 때, 내용 지워짐");
  add_button(sd_format_row, "포맷", sd_format_clicked, &sd_format_btn.label, true);
  add_note(p, "FAT32 형식 카드만 읽습니다. 64GB 이상 카드는 보통 exFAT라서 \"카드 포맷\"이 필요합니다. 카드가 있으면 PC 없이도(블루투스만 연결되어 있어도) 녹화할 수 있습니다. 사진은 /photos, 동영상은 /videos 에 저장됩니다.");
}

static void build_general(lv_obj_t *parent) {
  lv_obj_t *p = add_sub_page(parent, PAGE_GENERAL, "일반");
  lv_label_set_text(add_info(p, "이름"), "LLM Deck");
  lv_label_set_text(add_info(p, "펌웨어"), __DATE__ " " __TIME__);
  uptime_value = add_info(p, "켜진 지");
  add_button(add_row(p, "기기 재시작", nullptr), "재시작", restart_clicked, &restart_btn.label, true);
}

void settings_build(lv_obj_t *parent) {
  lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
  build_list(parent);
  build_wifi(parent);
  build_ble(parent);
  build_speaker(parent);
  build_notify(parent);
  build_display(parent);
  build_battery(parent);
  build_camera(parent);
  build_sd(parent);
  build_general(parent);
  // 켤 때는 값만 숨기고 보이기만 한다 (SD 카드 등은 설정을 열 때 읽는다)
  lv_obj_clear_flag(pages[PAGE_LIST], LV_OBJ_FLAG_HIDDEN);
}

// ---- 상태 글자 고치기 ----

static void set_text(lv_obj_t *lbl, const char *text) {
  if (lbl && strcmp(lv_label_get_text(lbl), text) != 0) lv_label_set_text(lbl, text);
}

static const char *wifi_text() {
  static char buf[40];
  if (!settings_link_allowed(LINK_WIFI) && !settings_link_allowed(LINK_CLOUD)) return "끔";
  if (!WiFi.isConnected()) return "연결 안 됨";
  strlcpy(buf, WiFi.SSID().c_str(), sizeof(buf));
  return buf;
}

static const char *ble_text() {
  static char buf[24];
  if (!ble_link_enabled()) return "끔";
  int n = ble_peer_count();
  if (n == 0) return "PC 기다리는 중";
  snprintf(buf, sizeof(buf), "PC %d대 연결", n);
  return buf;
}

static const char *spk_text(bool shortly) {
  static char buf[48];
  int st = speaker_state();
  char name[SPK_NAME_LEN] = "";
  bool target = speaker_target(name, sizeof(name));
  if (st < 0) return "모듈 없음";
  switch (st) {
    case SPK_STATE_FINDING:
      return "찾는 중";
    case SPK_STATE_CONNECTING:
      return "연결 중";
    case SPK_STATE_CONNECTED:
    case SPK_STATE_PLAYING:
      if (shortly) return "연결됨";
      snprintf(buf, sizeof(buf), "%s 연결됨", name);
      return buf;
    default:
      if (!target) return shortly ? "없음" : "기억한 스피커 없음";
      return shortly ? "쉬는 중" : "쉬는 중 (소리 낼 때 연결)";
  }
}

static void refresh_list() {
  set_text(spk_value, spk_text(true));
  set_text(wifi_value, wifi_text());
  // 목록에서는 짧게 (자세한 것은 블루투스 화면에)
  char ble_short[16];
  int peers = ble_peer_count();
  if (!ble_link_enabled()) {
    strlcpy(ble_short, "끔", sizeof(ble_short));
  } else if (peers == 0) {
    strlcpy(ble_short, "대기 중", sizeof(ble_short));
  } else {
    snprintf(ble_short, sizeof(ble_short), "PC %d대", peers);
  }
  set_text(ble_value, ble_short);
  char buf[24];
  int pct = battery_pct();
  if (pct < 0) {
    set_text(battery_value, "없음");
  } else {
    snprintf(buf, sizeof(buf), "%s%d%%", battery_charging() ? LV_SYMBOL_CHARGE " " : "", pct);
    set_text(battery_value, buf);
  }
}

// 1초마다 바뀔 수 있는 것 (SD 카드는 읽는 데 시간이 걸려 화면을 열 때와 버튼을 눌렀을 때만)
static void refresh_live() {
  char buf[48];
  switch (current) {
    case PAGE_LIST:
      refresh_list();
      break;
    case PAGE_WIFI:
      set_text(wifi_status, wifi_text());
      set_text(wifi_ip, WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "-");
      if (WiFi.isConnected()) {
        int rssi = WiFi.RSSI();
        snprintf(buf, sizeof(buf), "%s (%ddBm)", rssi > -60 ? "강함" : rssi > -72 ? "보통" : "약함", rssi);
        set_text(wifi_signal, buf);
      } else {
        set_text(wifi_signal, "-");
      }
      break;
    case PAGE_BLE: {
      set_text(ble_status, ble_text());
      bool any = false;
      for (int i = 0; i < BLE_MAX_PEERS; i++) {
        char name[32] = "";
        int state = ble_peer_get(i, name, sizeof(name));
        if (state == 0) {
          lv_obj_add_flag(ble_rows[i], LV_OBJ_FLAG_HIDDEN);
          continue;
        }
        any = true;
        set_text(ble_names[i], name);
        set_text(ble_states[i], state == 2 ? "사용 중" : "기다리는 중 (눌러서 사용)");
        lv_obj_set_style_text_color(ble_states[i], state == 2 ? COLOR_ACCENT : COLOR_DIM, 0);
        lv_obj_clear_flag(ble_rows[i], LV_OBJ_FLAG_HIDDEN);
      }
      if (any) {
        lv_obj_add_flag(ble_none, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_clear_flag(ble_none, LV_OBJ_FLAG_HIDDEN);
      }
      break;
    }
    case PAGE_SPEAKER: {
      set_text(spk_status, spk_text(false));
      char name[SPK_NAME_LEN] = "";
      set_text(spk_target, speaker_target(name, sizeof(name)) ? name : "-");
      bool finding = speaker_state() == SPK_STATE_FINDING;
      set_text(spk_find_lbl, finding ? "찾는 중" : "찾기");
      int n = speaker_found_count();
      for (int i = 0; i < SPK_MAX_FOUND; i++) {
        int rssi = 0;
        if (!speaker_found(i, name, sizeof(name), &rssi)) {
          lv_obj_add_flag(spk_rows[i], LV_OBJ_FLAG_HIDDEN);
          continue;
        }
        set_text(spk_names[i], name);
        snprintf(buf, sizeof(buf), "눌러서 연결 (신호 %s)", rssi > -65 ? "강함" : rssi > -80 ? "보통" : "약함");
        set_text(spk_signals[i], buf);
        lv_obj_clear_flag(spk_rows[i], LV_OBJ_FLAG_HIDDEN);
      }
      if (!speaker_present()) {
        set_text(spk_none, "스피커 모듈이 연결되어 있지 않습니다.");
      } else if (finding && n == 0) {
        set_text(spk_none, "찾는 중입니다. 스피커를 켜고 페어링 모드로 두세요.");
      } else if (n == 0) {
        set_text(spk_none, "\"찾기\"를 누르면 주변 스피커가 여기 나옵니다.");
      } else {
        set_text(spk_none, "연결할 스피커를 누르세요.");
      }
      break;
    }
    case PAGE_BATTERY: {
      int pct = battery_pct();
      if (pct < 0) {
        set_text(bat_level, "배터리 없음");
        set_text(bat_state, battery_external_power() ? "USB" : "-");
      } else {
        snprintf(buf, sizeof(buf), "%d%% (%.2fV)", pct, battery_mv() / 1000.0f);
        set_text(bat_level, buf);
        set_text(bat_state, battery_charging()           ? "충전 중"
                            : battery_external_power() ? "USB (충전 완료)"
                                                       : "배터리 사용 중");
      }
      break;
    }
    case PAGE_GENERAL: {
      uint32_t min = millis() / 60000;
      snprintf(buf, sizeof(buf), "%lu시간 %lu분", (unsigned long)(min / 60), (unsigned long)(min % 60));
      set_text(uptime_value, buf);
      break;
    }
    default:
      break;
  }
}

static void refresh_sd(bool count) {
  uint32_t total, used;
  if (!storage_info(&total, &used)) {
    set_text(sd_status, "없음 또는 FAT32 아님");
    if (sd_format_row) lv_obj_clear_flag(sd_format_row, LV_OBJ_FLAG_HIDDEN);
    set_text(sd_space, "-");
    set_text(sd_count, "-");
    set_text(sd_vcount, "-");
    set_text(sd_value, "읽을 수 없음");
    return;
  }
  char buf[40];
  set_text(sd_status, "사용 가능");
  if (sd_format_row) lv_obj_add_flag(sd_format_row, LV_OBJ_FLAG_HIDDEN);
  if (total >= 1024) {
    snprintf(buf, sizeof(buf), "%.1f / %.1fGB", used / 1024.0f, total / 1024.0f);
  } else {
    snprintf(buf, sizeof(buf), "%lu / %luMB", (unsigned long)used, (unsigned long)total);
  }
  set_text(sd_space, buf);
  if (count) {
    snprintf(buf, sizeof(buf), "%d장", storage_count("/photos"));
    set_text(sd_count, buf);
    snprintf(buf, sizeof(buf), "%d개", storage_count("/videos"));
    set_text(sd_vcount, buf);
  }
  snprintf(buf, sizeof(buf), "%luGB", (unsigned long)((total + 512) / 1024));
  set_text(sd_value, total >= 1024 ? buf : "있음");
}

static void refresh_page() {
  switch (current) {
    case PAGE_LIST:
      refresh_sd(false);  // 목록 오른쪽의 "없음/32GB"
      break;
    case PAGE_WIFI: {
      String names = net_saved_names();
      lv_label_set_text(wifi_list, names.length() ? names.c_str() : "없음");
      break;
    }
    case PAGE_DISPLAY:
      lv_slider_set_value(bright_slider, brightness, LV_ANIM_OFF);
      lv_label_set_text_fmt(bright_value, "%d%%", brightness);
      show_saver();
      break;
    case PAGE_CAMERA:
      show_camera_mount();
      set_checked(mirror_sw, camera_mount_mirror());
      break;
    case PAGE_SD:
      refresh_sd(true);
      break;
    case PAGE_SPEAKER:
      show_spk_vol();
      break;
    default:
      break;
  }
  refresh_live();
}

void settings_refresh() {
  // 설정을 열 때마다 목록부터 (PC 에서 바뀐 값도 맞춘다)
  for (LinkOption &o : link_options) set_checked(o.sw, o.allowed);
  set_checked(rotate_sw, auto_rotate);
  set_checked(dim_sw, auto_dim);
  set_checked(alert_sw, limit_alerts);
  set_checked(sound_sw, mic_sound);
  set_checked(sd_sw, sd_photos);
  set_checked(sd_video_sw, sd_videos);
  set_checked(spk_on_sw, spk_on);
  set_checked(spk_keep_sw, spk_keep);
  show_page(PAGE_LIST);
}

void settings_redraw() {
  refresh_live();
}

void settings_loop() {
  static uint32_t checked_ms = 0;
  if (millis() - checked_ms < 1000) return;
  checked_ms = millis();
  refresh_live();
}

bool settings_back() {
  if (current == PAGE_LIST) return false;
  show_page(PAGE_LIST);
  return true;
}

void settings_open(const char *name) {
  for (int i = 0; i < PAGE_COUNT; i++) {
    if (strcmp(name, PAGE_KEYS[i]) == 0) {
      show_page((Page)i);
      return;
    }
  }
  // "end": 지금 화면의 맨 아래까지 내린다 (진단용 화면 찍기)
  if (strcmp(name, "end") == 0) lv_obj_scroll_to_y(pages[current], LV_COORD_MAX, LV_ANIM_OFF);
}
