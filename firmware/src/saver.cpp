#include "saver.h"

#include <esp_random.h>
#include <lvgl.h>
#include <time.h>

#include "display.h"
#include "mic.h"
#include "ui.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

// 같은 자리에 오래 있지 않도록 이 간격마다 옮긴다
#define MOVE_MS 30000

static lv_obj_t *overlay;
static lv_obj_t *box;
static lv_obj_t *time_lbl;
static lv_obj_t *date_lbl;
static lv_obj_t *usage_lbl;
static lv_obj_t *mic_lbl;
static bool visible = false;
static uint32_t moved_ms = 0;
static uint32_t demo_touch_ms = 0;
static bool demo = false;

static lv_obj_t *add_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, "");
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, color, 0);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  return lbl;
}

void saver_init() {
  // 카메라 화면보다 위(같은 층에서 나중에 만든 것이 위)
  overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(overlay);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

  box = lv_obj_create(overlay);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(box, 2, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

  // 밝은 흰색 대신 회색으로 그려 같은 점이 오래 밝게 켜져 있지 않게 한다
  time_lbl = add_label(box, &lv_font_montserrat_48, lv_color_hex(0xB0B6BD));
  date_lbl = add_label(box, &font_ko_20, lv_color_hex(0x7A828C));
  usage_lbl = add_label(box, &font_ko_14, lv_color_hex(0x7A828C));
  mic_lbl = add_label(box, &font_ko_14, lv_color_hex(0xC8372D));
  lv_label_set_text(mic_lbl, LV_SYMBOL_AUDIO " 마이크 켜짐");

  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_text() {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    static const char *DAYS[] = {"일", "월", "화", "수", "목", "금", "토"};
    struct tm t;
    localtime_r(&now, &t);
    lv_label_set_text_fmt(time_lbl, "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text_fmt(date_lbl, "%d월 %d일 %s요일", t.tm_mon + 1, t.tm_mday, DAYS[t.tm_wday]);
  } else {
    // 시계를 아직 못 맞췄으면 시각 대신 이름만
    lv_label_set_text(time_lbl, "--:--");
    lv_label_set_text(date_lbl, "LLM Deck");
  }
  char usage[160];
  ui_usage_summary(usage, sizeof(usage));
  lv_label_set_text(usage_lbl, usage);
  if (usage[0]) {
    lv_obj_clear_flag(usage_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(usage_lbl, LV_OBJ_FLAG_HIDDEN);
  }
  if (mic_is_on()) {
    lv_obj_clear_flag(mic_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(mic_lbl, LV_OBJ_FLAG_HIDDEN);
  }
}

// 화면 안에서 아무 자리로 옮긴다
static void move_box() {
  int w = display_width(), h = display_height();
  lv_obj_set_size(overlay, w, h);
  lv_obj_update_layout(overlay);
  int room_x = LV_MAX(0, w - lv_obj_get_width(box));
  int room_y = LV_MAX(0, h - lv_obj_get_height(box));
  lv_obj_set_pos(box, esp_random() % (room_x + 1), esp_random() % (room_y + 1));
  moved_ms = millis();
}

void saver_set_visible(bool on) {
  if (on == visible) return;
  visible = on;
  if (on) {
    refresh_text();
    move_box();
    lv_obj_move_foreground(overlay);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  }
}

void saver_layout() {
  if (visible) move_box();
}

bool saver_visible() {
  return visible;
}

void saver_loop() {
  static uint32_t text_ms = 0;
  if (!visible) return;
  if (millis() - text_ms > 1000) {
    text_ms = millis();
    refresh_text();
  }
  if (millis() - moved_ms > MOVE_MS) move_box();
}

void saver_demo() {
  demo = true;
  demo_touch_ms = display_last_touch_ms();
}

bool saver_demo_active() {
  if (demo && display_last_touch_ms() != demo_touch_ms) demo = false;
  return demo;
}
