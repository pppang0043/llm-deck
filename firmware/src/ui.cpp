#include "ui.h"

#include <lvgl.h>

#include "pins.h"

#define STATUS_H 24
#define MIC_H 56
#define LEVEL_H 4

#define COLOR_BG lv_color_hex(0x101418)
#define COLOR_DIM lv_color_hex(0x8A939E)
#define COLOR_ACCENT lv_color_hex(0xD97757)
#define COLOR_WARN lv_color_hex(0xE0A030)
#define COLOR_FULL lv_color_hex(0xE0483C)
#define COLOR_MIC_OFF lv_color_hex(0x2A3038)
#define COLOR_MIC_ON lv_color_hex(0xC8372D)

static lv_obj_t *status_lbl;
static lv_obj_t *tiles;
static lv_obj_t *level_bar;
static lv_obj_t *mic_btn;
static lv_obj_t *mic_lbl;
static void (*mic_toggle_cb)();
static bool waiting = true;
static char wifi_info[48] = "starting...";

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, color, 0);
  return lbl;
}

// 왼쪽과 오른쪽에 글자가 하나씩 있는 한 줄
static void add_row(lv_obj_t *parent, const char *left, const lv_font_t *left_font, lv_color_t left_color,
                    const char *right, const lv_font_t *right_font, lv_color_t right_color) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_align(add_label(row, left, left_font, left_color), LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_align(add_label(row, right, right_font, right_color), LV_ALIGN_RIGHT_MID, 0, 0);
}

// 한도 한 구간: 왼쪽에 이름과 초기화 시각, 오른쪽에 큰 숫자, 아래에 막대
static void add_gauge(lv_obj_t *parent, JsonObjectConst bar) {
  int pct = constrain(bar["pct"] | 0, 0, 100);
  lv_color_t color = pct >= 90 ? COLOR_FULL : pct >= 70 ? COLOR_WARN : lv_color_white();

  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_align(add_label(row, bar["label"] | "", &lv_font_montserrat_28, COLOR_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_align(add_label(row, bar["note"] | "", &lv_font_montserrat_14, COLOR_DIM), LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_align(add_label(row, bar["text"] | "", &lv_font_montserrat_48, color), LV_ALIGN_RIGHT_MID, 0, 0);

  lv_obj_t *b = lv_bar_create(parent);
  lv_obj_set_size(b, lv_pct(100), 12);
  lv_bar_set_range(b, 0, 100);
  lv_bar_set_value(b, pct, LV_ANIM_OFF);
  if (pct >= 70) lv_obj_set_style_bg_color(b, color, LV_PART_INDICATOR);
}

static lv_obj_t *add_page(uint8_t index) {
  lv_obj_t *page = lv_tileview_add_tile(tiles, index, 0, LV_DIR_HOR);
  lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(page, 10, 0);
  lv_obj_set_style_pad_row(page, 6, 0);
  return page;
}

static void show_placeholder(const char *title, const char *body) {
  lv_obj_clean(tiles);
  lv_obj_t *page = add_page(0);
  add_label(page, title, &lv_font_montserrat_20, COLOR_ACCENT);
  lv_obj_t *lbl = add_label(page, body, &lv_font_montserrat_14, COLOR_DIM);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, lv_pct(100));
}

// PC 에서 첫 데이터가 오기 전까지 보여주는 화면: 연결 방법 안내
static void show_waiting() {
  char body[200];
  snprintf(body, sizeof(body),
           "Waiting for the PC companion app.\n\nUSB: plug in the cable\nBluetooth: " "LLM-Deck" "\nWi-Fi: %s", wifi_info);
  show_placeholder("LLM Deck", body);
}

static void mic_btn_event(lv_event_t *e) {
  if (mic_toggle_cb) mic_toggle_cb();
}

void ui_init(void (*on_mic_toggle)()) {
  mic_toggle_cb = on_mic_toggle;

  lv_disp_t *disp = lv_disp_get_default();
  lv_disp_set_theme(disp, lv_theme_default_init(disp, COLOR_ACCENT, COLOR_DIM, true, LV_FONT_DEFAULT));

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  status_lbl = add_label(scr, "Starting...", &lv_font_montserrat_14, COLOR_DIM);
  lv_obj_align(status_lbl, LV_ALIGN_TOP_LEFT, 8, 4);

  tiles = lv_tileview_create(scr);
  lv_obj_set_size(tiles, LCD_W, LCD_H - STATUS_H - LEVEL_H - MIC_H);
  lv_obj_set_pos(tiles, 0, STATUS_H);
  lv_obj_set_style_bg_color(tiles, COLOR_BG, 0);
  show_waiting();

  level_bar = lv_bar_create(scr);
  lv_obj_set_size(level_bar, LCD_W, LEVEL_H);
  lv_obj_set_pos(level_bar, 0, LCD_H - MIC_H - LEVEL_H);
  lv_obj_set_style_radius(level_bar, 0, 0);
  lv_obj_set_style_radius(level_bar, 0, LV_PART_INDICATOR);
  lv_bar_set_range(level_bar, 0, 100);

  mic_btn = lv_btn_create(scr);
  lv_obj_set_size(mic_btn, LCD_W, MIC_H);
  lv_obj_set_pos(mic_btn, 0, LCD_H - MIC_H);
  lv_obj_set_style_radius(mic_btn, 0, 0);
  lv_obj_add_event_cb(mic_btn, mic_btn_event, LV_EVENT_CLICKED, NULL);
  mic_lbl = add_label(mic_btn, "", &lv_font_montserrat_20, lv_color_white());
  lv_obj_center(mic_lbl);
  ui_set_mic(false);
}

void ui_set_status(const char *text) {
  lv_label_set_text(status_lbl, text);
}

void ui_set_wifi_info(const char *text) {
  strlcpy(wifi_info, text, sizeof(wifi_info));
  if (waiting) show_waiting();
}

void ui_set_usage(JsonArrayConst services) {
  waiting = false;

  // 데이터가 갱신돼도 보고 있던 페이지에 머물도록 현재 위치를 기억해 둔다
  lv_obj_t *active = lv_tileview_get_tile_act(tiles);
  uint8_t keep = active ? lv_obj_get_x(active) / LCD_W : 0;

  size_t count = services.size();
  if (count == 0) {
    show_placeholder("No services", "The companion app sent no usage data. Check its config.");
    return;
  }

  lv_obj_clean(tiles);
  uint8_t index = 0;
  for (JsonObjectConst svc : services) {
    lv_obj_t *page = add_page(index);

    // 제목 오른쪽의 작은 글자는 이 값이 언제 것인지(시각)를 나타낸다
    add_row(page, svc["name"] | "?", &lv_font_montserrat_28, COLOR_ACCENT, svc["note"] | "", &lv_font_montserrat_14,
            COLOR_DIM);

    if (svc["headline"].is<const char *>()) {
      add_label(page, svc["headline"], &lv_font_montserrat_48, lv_color_white());
    }
    if (svc["sub"].is<const char *>()) {
      lv_obj_t *sub = add_label(page, svc["sub"], &lv_font_montserrat_14, COLOR_DIM);
      lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(sub, lv_pct(100));
    }
    for (JsonObjectConst bar : svc["bars"].as<JsonArrayConst>()) {
      add_gauge(page, bar);
    }
    for (JsonArrayConst row : svc["rows"].as<JsonArrayConst>()) {
      add_row(page, row[0] | "", &lv_font_montserrat_20, COLOR_DIM, row[1] | "", &lv_font_montserrat_20,
              lv_color_white());
    }
    index++;
  }

  lv_obj_set_tile_id(tiles, keep < count ? keep : count - 1, 0, LV_ANIM_OFF);
}

void ui_set_mic(bool on) {
  lv_obj_set_style_bg_color(mic_btn, on ? COLOR_MIC_ON : COLOR_MIC_OFF, 0);
  lv_label_set_text(mic_lbl, on ? LV_SYMBOL_AUDIO "  MIC ON" : LV_SYMBOL_MUTE "  MIC OFF");
  if (!on) ui_set_level(0);
}

void ui_set_level(uint8_t pct) {
  lv_bar_set_value(level_bar, pct, LV_ANIM_OFF);
}
