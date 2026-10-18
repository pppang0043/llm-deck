#include "ui.h"

#include <lvgl.h>

#include "psram_json.h"
#include "askpanel.h"
#include "display.h"
#include "history.h"
#include "orcapanel.h"
#include "pcpanel.h"
#include "settings.h"

// 한글이 들어 있는 글꼴 (src/fonts, 나눔고딕). 큰 숫자(48)는 기본 글꼴을 쓴다.
LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);
LV_FONT_DECLARE(font_ko_28);

// 상태줄 높이. 오른쪽 끝 돌리기 버튼을 누르기 쉽게 넉넉히 둔다.
#define STATUS_H 32
#define MIC_H 56
// 가로 화면에서는 마이크 버튼이 오른쪽에 세로로 놓인다
#define MIC_W 72
#define LEVEL_H 4
// 상태줄 건전지 모양 크기
#define BAT_W 26
#define BAT_H 14

#define COLOR_BG lv_color_hex(0x101418)
#define COLOR_DIM lv_color_hex(0x8A939E)
#define COLOR_ACCENT lv_color_hex(0xD97757)
#define COLOR_WARN lv_color_hex(0xE0A030)
#define COLOR_FULL lv_color_hex(0xE0483C)
#define COLOR_MIC_OFF lv_color_hex(0x2A3038)
#define COLOR_MIC_ON lv_color_hex(0xC8372D)
#define COLOR_CHARGE lv_color_hex(0x4CAF50)

static lv_obj_t *status_lbl;
static lv_obj_t *tiles;
static lv_obj_t *level_bar;
static lv_obj_t *mic_btn;
static lv_obj_t *mic_lbl;
static void (*mic_toggle_cb)();
static void (*rotate_cb)();
static lv_obj_t *rotate_btn;
static lv_obj_t *clock_lbl;
static lv_obj_t *bat_icon;
static lv_obj_t *bat_fill;
static lv_obj_t *bat_bolt;
static lv_obj_t *bat_lbl;
static char clock_date[24] = "";
static char clock_time[8] = "";
static bool waiting = true;
static bool mic_on = false;
// 화면을 돌릴 때 다시 그릴 수 있도록 마지막으로 받은 사용량을 가지고 있는다
static JsonDocument last_usage(PSRAM_JSON);
static char wifi_info[48] = "시작 중...";

// 화면 구성: 홈(아이콘) / 사용량 / 정보. 상태줄은 늘 위에 있다. 카메라는 camera_ui 가 위에 덮는다.
enum View { VIEW_HOME, VIEW_USAGE, VIEW_SETTINGS, VIEW_SHORTCUTS, VIEW_ASK, VIEW_ORCA };
static View view = VIEW_HOME;  // 켤 때 첫 화면은 홈
static lv_obj_t *home;
#define HOME_ICONS 6
static lv_obj_t *home_icons[HOME_ICONS];
static lv_obj_t *ask_icon_sym;  // 홈의 질문 아이콘 (말할 수 없으면 흐리게)
static int pc_tile = 0;  // 사용량 화면에서 PC 상태 페이지의 번호 (맨 마지막)
static void update_pc_watch();
static lv_obj_t *settings_panel;
static void (*camera_cb)() = nullptr;
static void apply_view();

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

// ---- 사용률 기록 그래프 (구간을 누르면 화면 전체에 최근 24시간) ----

struct GaugeKey {
  char service[32];
  char label[16];
};
#define MAX_GAUGES 8
static GaugeKey gauge_keys[MAX_GAUGES];
static int gauge_count = 0;
static lv_obj_t *hist_panel, *hist_title, *hist_chart, *hist_note, *hist_left, *hist_right;
static lv_chart_series_t *hist_series;
static lv_coord_t hist_points[HISTORY_SLOTS];

static void hist_close(lv_event_t *e) {
  lv_obj_add_flag(hist_panel, LV_OBJ_FLAG_HIDDEN);
}

static void hist_build() {
  hist_panel = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(hist_panel);
  lv_obj_set_style_bg_color(hist_panel, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(hist_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(hist_panel, 10, 0);
  lv_obj_add_flag(hist_panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(hist_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(hist_panel, hist_close, LV_EVENT_CLICKED, NULL);

  hist_title = add_label(hist_panel, "", &font_ko_20, COLOR_ACCENT);
  lv_obj_align(hist_title, LV_ALIGN_TOP_LEFT, 0, 0);
  hist_note = add_label(hist_panel, "", &font_ko_14, COLOR_DIM);
  lv_obj_align(hist_note, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  hist_chart = lv_chart_create(hist_panel);
  lv_obj_clear_flag(hist_chart, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(hist_chart, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_chart_set_type(hist_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(hist_chart, HISTORY_SLOTS);
  lv_chart_set_range(hist_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_div_line_count(hist_chart, 5, 0);  // 25% 마다 가로줄
  lv_obj_set_style_bg_color(hist_chart, lv_color_hex(0x1C232B), 0);
  lv_obj_set_style_border_width(hist_chart, 0, 0);
  lv_obj_set_style_line_color(hist_chart, lv_color_hex(0x2A323B), LV_PART_MAIN);
  lv_obj_set_style_size(hist_chart, 0, LV_PART_INDICATOR);  // 점은 그리지 않고 선만
  lv_obj_set_style_line_width(hist_chart, 2, LV_PART_ITEMS);
  hist_series = lv_chart_add_series(hist_chart, COLOR_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
  // 그래프 아래 양 끝: 시간 축
  hist_left = add_label(hist_panel, "24시간 전", &font_ko_14, COLOR_DIM);
  hist_right = add_label(hist_panel, "지금", &font_ko_14, COLOR_DIM);
  lv_chart_set_ext_y_array(hist_chart, hist_series, hist_points);
}

static void hist_show(const GaugeKey &k) {
  if (!hist_panel) hist_build();
  uint8_t v[HISTORY_SLOTS];
  bool have = history_get(k.service, k.label, v);
  int peak = -1, last = -1, known = 0;
  for (int i = 0; i < HISTORY_SLOTS; i++) {
    bool ok = have && v[i] != HISTORY_NONE;
    hist_points[i] = ok ? v[i] : LV_CHART_POINT_NONE;
    if (ok) {
      known++;
      last = v[i];
      if (v[i] > peak) peak = v[i];
    }
  }
  lv_label_set_text_fmt(hist_title, "%s %s", k.service, k.label);
  lv_label_set_long_mode(hist_title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(hist_title, display_width() - 20);
  if (known == 0) {
    lv_label_set_text(hist_note, "아직 기록이 없습니다 (10분마다 모읍니다). 누르면 닫힘");
  } else {
    lv_label_set_text_fmt(hist_note, "최고 %d%%, 지금 %d%%. 누르면 닫힘", peak, last);
  }
  // 화면 방향에 맞춰 크기를 다시 정한다
  lv_obj_set_size(hist_panel, display_width(), display_height());
  lv_obj_set_size(hist_chart, lv_pct(100), display_height() - 20 - 30 - 42);
  lv_obj_align(hist_chart, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_align_to(hist_left, hist_chart, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);
  lv_obj_align_to(hist_right, hist_chart, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
  lv_chart_refresh(hist_chart);
  lv_obj_clear_flag(hist_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(hist_panel);
}

static void gauge_clicked(lv_event_t *e) {
  int i = (intptr_t)lv_event_get_user_data(e);
  if (i >= 0 && i < gauge_count) hist_show(gauge_keys[i]);
}

bool ui_history_visible() {
  return hist_panel && !lv_obj_has_flag(hist_panel, LV_OBJ_FLAG_HIDDEN);
}

void ui_history_close() {
  if (hist_panel) lv_obj_add_flag(hist_panel, LV_OBJ_FLAG_HIDDEN);
}

// 한도 한 구간: 왼쪽에 이름과 초기화 시각, 오른쪽에 큰 숫자, 아래에 막대. 누르면 기록 그래프.
static void add_gauge(lv_obj_t *parent, const char *service, JsonObjectConst bar) {
  int pct = constrain(bar["pct"] | 0, 0, 100);
  lv_color_t color = pct >= 90 ? COLOR_FULL : pct >= 70 ? COLOR_WARN : lv_color_white();

  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_align(add_label(row, bar["label"] | "", &font_ko_28, COLOR_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_align(add_label(row, bar["note"] | "", &font_ko_14, COLOR_DIM), LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_align(add_label(row, bar["text"] | "", &lv_font_montserrat_48, color), LV_ALIGN_RIGHT_MID, 0, 0);
  if (gauge_count < MAX_GAUGES) {
    GaugeKey &k = gauge_keys[gauge_count];
    strlcpy(k.service, service, sizeof(k.service));
    strlcpy(k.label, bar["label"] | "", sizeof(k.label));
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, gauge_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)gauge_count);
    gauge_count++;
  }

  lv_obj_t *b = lv_bar_create(parent);
  lv_obj_set_size(b, lv_pct(100), 12);
  lv_bar_set_range(b, 0, 100);
  lv_bar_set_value(b, pct, LV_ANIM_OFF);
  if (pct >= 70) lv_obj_set_style_bg_color(b, color, LV_PART_INDICATOR);
}

static lv_obj_t *add_page(uint8_t index) {
  // 서비스 페이지는 위아래로 넘긴다 (오른쪽으로 밀기는 홈으로 가기에 쓴다)
  lv_obj_t *page = lv_tileview_add_tile(tiles, 0, index, LV_DIR_VER);
  lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(page, 10, 0);
  lv_obj_set_style_pad_row(page, 6, 0);
  return page;
}

// 사용량 페이지 맨 끝에 PC 상태 페이지를 붙인다 (pcpanel.cpp 가 그린다)
static void add_pc_tile(uint8_t index) {
  lv_obj_t *tile = lv_tileview_add_tile(tiles, 0, index, LV_DIR_VER);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  pcpanel_status_attach(tile, lv_obj_get_width(tiles), lv_obj_get_height(tiles));
  pc_tile = index;
}

static void show_placeholder(const char *title, const char *body) {
  pcpanel_status_detach();
  lv_obj_clean(tiles);
  lv_obj_t *page = add_page(0);
  add_label(page, title, &font_ko_20, COLOR_ACCENT);
  lv_obj_t *lbl = add_label(page, body, &font_ko_14, COLOR_DIM);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, lv_pct(100));
  add_pc_tile(1);
  update_pc_watch();
}

// PC 에서 첫 데이터가 오기 전까지 보여주는 화면: 연결 방법 안내
static void show_waiting() {
  char body[200];
  snprintf(body, sizeof(body),
           "PC 도우미를 기다리는 중\n\nUSB: 케이블 연결\n블루투스: LLM-Deck\nWi-Fi: %s", wifi_info);
  show_placeholder("LLM Deck", body);
}

// 사용량 화면 아래 버튼: 받아쓰기. 한 번 누르고 PC 마이크로 말한 뒤 다시 누르면, 받아 적은 글이
// PC 의 커서 자리에 입력된다 (Claude 에 묻지 않는다). 질문은 홈의 질문 화면에서 한다.
static void mic_btn_event(lv_event_t *e) {
  if (askpanel_talk_state() != ASK_TALKING) {
    askpanel_press(true);
  } else if (askpanel_dictating()) {
    askpanel_release();
  }
}

void ui_refresh_talk() {
  orcapanel_refresh();
  if (!mic_btn) return;
  int st = askpanel_talk_state();
  bool wide = display_width() > display_height();
  lv_obj_set_style_bg_color(mic_btn, st == ASK_TALKING ? COLOR_MIC_ON : st == ASK_READY ? lv_color_hex(0x2E7D5B)
                                                                                      : COLOR_MIC_OFF, 0);
  lv_obj_set_style_text_color(mic_lbl, st == ASK_READY || st == ASK_TALKING ? lv_color_white() : COLOR_DIM, 0);
  // 가로 화면의 버튼은 좁아서 여러 줄로 쓴다
  const char *text;
  int step = askpanel_dictate_step();
  if (st == ASK_TALKING && step == DICTATE_PREPARING) {
    text = wide ? "마이크\n준비\n중..." : "마이크 준비 중...";
  } else if (st == ASK_TALKING) {
    text = wide ? LV_SYMBOL_AUDIO "\n말하세요\n\n다시\n누르면\n입력" : LV_SYMBOL_AUDIO "  말하세요... 다시 누르면 입력";
  } else if (step == DICTATE_WRITING) {
    text = wide ? "받아\n적는\n중..." : "받아 적는 중...";
  } else if (st == ASK_NO_PC) {
    text = wide ? "PC\n연결\n없음" : "PC 연결 없음";
  } else if (st == ASK_NO_MIC) {
    text = wide ? "PC\n마이크\n없음" : "PC 마이크 없음";
  } else {
    text = wide ? LV_SYMBOL_AUDIO "\n받아\n쓰기\n\n눌러서\n말하기" : LV_SYMBOL_AUDIO "  눌러서 받아쓰기";
  }
  lv_label_set_text(mic_lbl, text);
  lv_obj_center(mic_lbl);
  if (ask_icon_sym) {
    lv_obj_set_style_bg_color(lv_obj_get_parent(ask_icon_sym), st == ASK_NO_PC || st == ASK_NO_MIC ? COLOR_MIC_OFF
                                                                                              : lv_color_hex(0x2E7D5B), 0);
    lv_obj_set_style_text_color(ask_icon_sym, st == ASK_NO_PC || st == ASK_NO_MIC ? COLOR_DIM : lv_color_white(), 0);
  }
}

static void rotate_btn_event(lv_event_t *e) {
  if (rotate_cb) rotate_cb();
}

static void screen_gesture(lv_event_t *e) {
  // 어디서든 오른쪽으로 밀면 홈으로
  // 설정의 분류 화면에서는 먼저 목록으로 돌아간다 (아이폰의 뒤로 가기처럼)
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) != LV_DIR_RIGHT || view == VIEW_HOME) return;
  if (view == VIEW_SETTINGS && settings_back()) return;
  ui_show_home();
}

static void home_icon_event(lv_event_t *e) {
  switch ((intptr_t)lv_event_get_user_data(e)) {
    case 0:
      view = VIEW_USAGE;
      apply_view();
      break;
    case 1:
      if (camera_cb) camera_cb();
      break;
    case 2:
      view = VIEW_ASK;
      apply_view();
      break;
    case 3:
      view = VIEW_SHORTCUTS;
      apply_view();
      break;
    case 4:
      view = VIEW_ORCA;
      apply_view();
      break;
    case 5:
      view = VIEW_SETTINGS;
      apply_view();
      break;
    default:
      break;
  }
}

// 홈 아이콘: 둥근 네모 안에 그림, 아래에 이름
static lv_obj_t *add_home_icon(intptr_t index, const char *symbol, const char *name, lv_color_t color, bool enabled) {
  lv_obj_t *cell = lv_obj_create(home);
  lv_obj_remove_style_all(cell);
  lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(cell, home_icon_event, LV_EVENT_CLICKED, (void *)index);

  lv_obj_t *tile = lv_obj_create(cell);
  lv_obj_remove_style_all(tile);
  lv_obj_set_size(tile, 56, 56);
  lv_obj_set_style_radius(tile, 14, 0);
  lv_obj_set_style_bg_color(tile, enabled ? color : COLOR_MIC_OFF, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(tile, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_t *sym = add_label(tile, symbol, &lv_font_montserrat_28, enabled ? lv_color_white() : COLOR_DIM);
  lv_obj_center(sym);

  lv_obj_t *lbl = add_label(cell, name, &font_ko_14, enabled ? lv_color_white() : COLOR_DIM);
  lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
  home_icons[index] = cell;
  if (index == 2) ask_icon_sym = sym;
  return tile;
}

static void build_home() {
  home = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(home);
  lv_obj_set_style_bg_color(home, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
  lv_obj_clear_flag(home, LV_OBJ_FLAG_SCROLLABLE);
  add_home_icon(0, LV_SYMBOL_LIST, "사용량", COLOR_ACCENT, true);
  add_home_icon(1, LV_SYMBOL_IMAGE, "카메라", lv_color_hex(0x3A7BD5), true);
  add_home_icon(2, LV_SYMBOL_AUDIO, "질문", lv_color_hex(0x2E7D5B), true);
  add_home_icon(3, LV_SYMBOL_KEYBOARD, "단축키", lv_color_hex(0x7E57C2), true);
  add_home_icon(4, LV_SYMBOL_EYE_OPEN, "ORCA", lv_color_hex(0x1E6FB8), true);
  add_home_icon(5, LV_SYMBOL_SETTINGS, "설정", lv_color_hex(0x6B6F7A), true);

  // 설정 화면 (settings.cpp)
  settings_panel = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(settings_panel);
  lv_obj_set_style_bg_color(settings_panel, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(settings_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(settings_panel, 10, 0);
  settings_build(settings_panel);

  // 단축 버튼, PC 상태 (pcpanel.cpp)
  pcpanel_build(lv_scr_act());
  // 말로 묻고 글로 답받기 (askpanel.cpp)
  askpanel_build(lv_scr_act());
  // ORCA 세션 상태와 그 세션에 받아쓰기 (orcapanel.cpp)
  orcapanel_build(lv_scr_act());
}

// 홈 아이콘 배치: 가로 화면 3x2, 세로 화면 2x3
static void layout_home() {
  int w = display_width(), h = display_height() - STATUS_H;
  lv_obj_set_size(home, w, h);
  lv_obj_set_pos(home, 0, STATUS_H);
  lv_obj_set_size(settings_panel, w, h);
  lv_obj_set_pos(settings_panel, 0, STATUS_H);
  pcpanel_layout(0, STATUS_H, w, h);
  askpanel_layout(0, STATUS_H, w, h);
  orcapanel_layout(0, STATUS_H, w, h);
  int cols = w > display_height() ? 3 : 2;
  int rows = (HOME_ICONS + cols - 1) / cols;
  int cw = w / cols, ch = h / rows;
  for (int i = 0; i < HOME_ICONS; i++) {
    lv_obj_set_size(home_icons[i], cw, 82);
    lv_obj_set_pos(home_icons[i], (i % cols) * cw, (i / cols) * ch + (ch - 82) / 2);
  }
}

static void apply_view() {
  bool usage = view == VIEW_USAGE;
  ui_history_close();
  lv_obj_t *usage_parts[] = {tiles, mic_btn};
  for (lv_obj_t *o : usage_parts) {
    if (usage) {
      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (view == VIEW_HOME) {
    lv_obj_clear_flag(home, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(home, LV_OBJ_FLAG_HIDDEN);
  }
  pcpanel_show_shortcuts(view == VIEW_SHORTCUTS);
  update_pc_watch();
  askpanel_show(view == VIEW_ASK);
  orcapanel_show(view == VIEW_ORCA);
  if (view == VIEW_SETTINGS) {
    settings_refresh();
    lv_obj_clear_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_HIDDEN);
  }
}

void ui_show_home() {
  view = VIEW_HOME;
  apply_view();
}

bool ui_settings_visible() {
  return view == VIEW_SETTINGS;
}

bool ui_info_visible() {
  return settings_info_visible();
}

void ui_set_info(const char *text) {
  settings_set_info(text);
}

void ui_set_home_actions(void (*open_camera)()) {
  camera_cb = open_camera;
}

void ui_init(void (*on_mic_toggle)(), void (*on_rotate)()) {
  mic_toggle_cb = on_mic_toggle;
  rotate_cb = on_rotate;

  lv_disp_t *disp = lv_disp_get_default();
  lv_disp_set_theme(disp, lv_theme_default_init(disp, COLOR_ACCENT, COLOR_DIM, true, LV_FONT_DEFAULT));

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  status_lbl = add_label(scr, "시작 중...", &font_ko_14, COLOR_DIM);
  lv_obj_align(status_lbl, LV_ALIGN_TOP_LEFT, 8, (STATUS_H - 16) / 2);

  // 화면 돌리기 버튼: 상태줄 오른쪽 끝에 작게. 누르기 쉽도록 반응 영역만 넓힌다.
  rotate_btn = lv_btn_create(scr);
  lv_obj_set_size(rotate_btn, 48, STATUS_H - 4);
  lv_obj_set_style_bg_color(rotate_btn, COLOR_MIC_OFF, 0);
  lv_obj_set_style_shadow_width(rotate_btn, 0, 0);
  lv_obj_set_style_pad_all(rotate_btn, 0, 0);
  lv_obj_set_ext_click_area(rotate_btn, 8);
  lv_obj_add_event_cb(rotate_btn, rotate_btn_event, LV_EVENT_CLICKED, NULL);
  lv_obj_center(add_label(rotate_btn, LV_SYMBOL_REFRESH, &lv_font_montserrat_20, lv_color_white()));

  clock_lbl = add_label(scr, "", &font_ko_14, COLOR_DIM);

  // 배터리: 숫자(%) + 건전지 모양. 배터리가 없으면 숨긴다.
  bat_lbl = add_label(scr, "", &font_ko_14, COLOR_DIM);
  bat_icon = lv_obj_create(scr);
  lv_obj_remove_style_all(bat_icon);
  lv_obj_set_size(bat_icon, BAT_W + 3, BAT_H);
  lv_obj_clear_flag(bat_icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *body = lv_obj_create(bat_icon);
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, BAT_W, BAT_H);
  lv_obj_set_style_border_width(body, 1, 0);
  lv_obj_set_style_border_color(body, COLOR_DIM, 0);
  lv_obj_set_style_radius(body, 2, 0);
  lv_obj_t *nub = lv_obj_create(bat_icon);
  lv_obj_remove_style_all(nub);
  lv_obj_set_size(nub, 2, BAT_H / 2);
  lv_obj_set_style_bg_color(nub, COLOR_DIM, 0);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
  lv_obj_align(nub, LV_ALIGN_RIGHT_MID, 0, 0);
  bat_fill = lv_obj_create(body);
  lv_obj_remove_style_all(bat_fill);
  lv_obj_set_style_bg_opa(bat_fill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bat_fill, 1, 0);
  lv_obj_set_pos(bat_fill, 2, 2);
  // 충전 중일 때 건전지 가운데에 번개
  lv_obj_add_flag(body, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  bat_bolt = add_label(body, LV_SYMBOL_CHARGE, &lv_font_montserrat_10, lv_color_white());
  lv_obj_center(bat_bolt);
  lv_obj_add_flag(bat_bolt, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bat_icon, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bat_lbl, LV_OBJ_FLAG_HIDDEN);

  tiles = lv_tileview_create(scr);
  lv_obj_set_style_bg_color(tiles, COLOR_BG, 0);
  lv_obj_add_event_cb(tiles, [](lv_event_t *e) { update_pc_watch(); }, LV_EVENT_VALUE_CHANGED, NULL);

  level_bar = lv_bar_create(scr);
  lv_obj_set_style_radius(level_bar, 0, 0);
  lv_obj_set_style_radius(level_bar, 0, LV_PART_INDICATOR);
  lv_bar_set_range(level_bar, 0, 100);
  // 기기 마이크(INMP441)의 소리 크기. 지금은 마이크를 달지 않아 쓰지 않는다.
  lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);

  mic_btn = lv_btn_create(scr);
  lv_obj_set_style_radius(mic_btn, 0, 0);
  lv_obj_add_event_cb(mic_btn, mic_btn_event, LV_EVENT_CLICKED, NULL);
  mic_lbl = add_label(mic_btn, "", &font_ko_20, lv_color_white());
  lv_obj_set_style_text_align(mic_lbl, LV_TEXT_ALIGN_CENTER, 0);

  build_home();
  lv_obj_add_event_cb(scr, screen_gesture, LV_EVENT_GESTURE, NULL);

  ui_layout();
  ui_set_mic(false);
  apply_view();
}

static void render_usage();

// 상태줄 오른쪽: [날짜 시각] [배터리 %] [건전지] [돌리기 버튼].
// 왼쪽 연결 방식 글자와 겹치면 날짜를 빼고 시각만 보인다.
static void layout_status_right() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_t *right = rotate_btn;
  if (!lv_obj_has_flag(bat_icon, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_update_layout(scr);
    lv_obj_align_to(bat_icon, rotate_btn, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_align_to(bat_lbl, bat_icon, LV_ALIGN_OUT_LEFT_MID, -3, 0);
    right = bat_lbl;
  }

  char text[40];
  snprintf(text, sizeof(text), "%s%s%s", clock_date, clock_date[0] ? " " : "", clock_time);
  lv_label_set_text(clock_lbl, text);
  lv_obj_update_layout(scr);
  lv_obj_align_to(clock_lbl, right, LV_ALIGN_OUT_LEFT_MID, -8, 0);
  lv_obj_update_layout(scr);
  if (clock_date[0] && lv_obj_get_x(clock_lbl) < lv_obj_get_x(status_lbl) + lv_obj_get_width(status_lbl) + 6) {
    lv_label_set_text(clock_lbl, clock_time);
    lv_obj_update_layout(scr);
    lv_obj_align_to(clock_lbl, right, LV_ALIGN_OUT_LEFT_MID, -8, 0);
  }
}

void ui_layout() {
  int w = display_width(), h = display_height();
  lv_obj_align(rotate_btn, LV_ALIGN_TOP_RIGHT, -2, 2);
  layout_status_right();
  if (w > h) {
    // 가로: 왼쪽에 페이지, 오른쪽에 마이크 버튼
    lv_obj_set_size(tiles, w - MIC_W, h - STATUS_H);
    lv_obj_set_pos(tiles, 0, STATUS_H);
    lv_obj_set_size(level_bar, MIC_W, LEVEL_H);
    lv_obj_set_pos(level_bar, w - MIC_W, STATUS_H);
    lv_obj_set_size(mic_btn, MIC_W, h - STATUS_H - LEVEL_H);
    lv_obj_set_pos(mic_btn, w - MIC_W, STATUS_H + LEVEL_H);
  } else {
    // 세로: 위에 페이지, 아래에 마이크 버튼
    lv_obj_set_size(tiles, w, h - STATUS_H - LEVEL_H - MIC_H);
    lv_obj_set_pos(tiles, 0, STATUS_H);
    lv_obj_set_size(level_bar, w, LEVEL_H);
    lv_obj_set_pos(level_bar, 0, h - MIC_H - LEVEL_H);
    lv_obj_set_size(mic_btn, w, MIC_H);
    lv_obj_set_pos(mic_btn, 0, h - MIC_H);
  }
  ui_refresh_talk();
  layout_home();
  // 페이지는 크기에 맞춰 새로 만든다
  if (waiting) {
    show_waiting();
  } else {
    render_usage();
  }
}

void ui_set_status(const char *text) {
  if (strcmp(lv_label_get_text(status_lbl), text) == 0) return;
  lv_label_set_text(status_lbl, text);
  layout_status_right();
  // 연결이 바뀌면 말하기 버튼도 (PC 없음 <-> 누르고 말하기)
  askpanel_refresh();
}

void ui_set_clock(const char *date, const char *time) {
  if (strcmp(clock_date, date) == 0 && strcmp(clock_time, time) == 0) return;
  strlcpy(clock_date, date, sizeof(clock_date));
  strlcpy(clock_time, time, sizeof(clock_time));
  layout_status_right();
}

void ui_set_battery(int pct, bool charging) {
  static int shown = -2;
  static bool shown_charging = false;
  if (pct == shown && charging == shown_charging) return;
  shown = pct;
  shown_charging = charging;
  if (pct < 0) {
    lv_obj_add_flag(bat_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bat_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_color_t color = charging ? COLOR_CHARGE : pct <= 10 ? COLOR_FULL : pct <= 20 ? COLOR_WARN : COLOR_CHARGE;
    int inner = BAT_W - 4;
    lv_obj_set_size(bat_fill, pct > 0 ? LV_MAX(1, inner * pct / 100) : 0, BAT_H - 4);
    lv_obj_set_style_bg_color(bat_fill, color, 0);
    lv_label_set_text_fmt(bat_lbl, "%d%%", pct);
    if (charging) {
      lv_obj_clear_flag(bat_bolt, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(bat_bolt, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_color(bat_lbl, !charging && pct <= 20 ? color : COLOR_DIM, 0);
    lv_obj_clear_flag(bat_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bat_lbl, LV_OBJ_FLAG_HIDDEN);
  }
  layout_status_right();
}

void ui_set_wifi_info(const char *text) {
  strlcpy(wifi_info, text, sizeof(wifi_info));
  if (waiting) show_waiting();
}

void ui_set_usage(JsonArrayConst services) {
  waiting = false;
  history_record(services);
  last_usage.set(services);
  render_usage();
}

static void render_usage() {
  JsonArrayConst services = last_usage.as<JsonArrayConst>();

  // 데이터가 갱신돼도 보고 있던 페이지에 머물도록 현재 위치를 기억해 둔다
  lv_obj_t *active = lv_tileview_get_tile_act(tiles);
  int tile_h = lv_obj_get_height(tiles);
  uint8_t keep = active && tile_h > 0 ? lv_obj_get_y(active) / tile_h : 0;

  size_t count = services.size();
  if (count == 0) {
    show_placeholder("표시할 내용 없음", "PC 도우미가 보낸 사용량이 없습니다.");
    return;
  }

  pcpanel_status_detach();
  lv_obj_clean(tiles);
  gauge_count = 0;
  uint8_t index = 0;
  for (JsonObjectConst svc : services) {
    lv_obj_t *page = add_page(index);

    // 제목 오른쪽의 작은 글자는 이 값이 언제 것인지(시각)를 나타낸다
    add_row(page, svc["name"] | "?", &font_ko_28, COLOR_ACCENT, svc["note"] | "", &font_ko_14,
            COLOR_DIM);

    if (svc["headline"].is<const char *>()) {
      add_label(page, svc["headline"], &lv_font_montserrat_48, lv_color_white());
    }
    if (svc["sub"].is<const char *>()) {
      lv_obj_t *sub = add_label(page, svc["sub"], &font_ko_14, COLOR_DIM);
      lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(sub, lv_pct(100));
    }
    for (JsonObjectConst bar : svc["bars"].as<JsonArrayConst>()) {
      add_gauge(page, svc["name"] | "?", bar);
    }
    for (JsonArrayConst row : svc["rows"].as<JsonArrayConst>()) {
      add_row(page, row[0] | "", &font_ko_20, COLOR_DIM, row[1] | "", &font_ko_20,
              lv_color_white());
    }
    index++;
  }
  add_pc_tile(index);

  lv_obj_set_tile_id(tiles, 0, keep <= count ? keep : count, LV_ANIM_OFF);
  update_pc_watch();
}

// PC 상태 페이지가 보이는 동안만 PC 에 3초마다 보내 달라고 한다
static void update_pc_watch() {
  lv_obj_t *active = lv_tileview_get_tile_act(tiles);
  int tile_h = lv_obj_get_height(tiles);
  bool on_pc = active && tile_h > 0 && lv_obj_get_y(active) / tile_h == pc_tile;
  pcpanel_show_status(view == VIEW_USAGE && on_pc);
}

void ui_usage_summary(char *out, size_t size) {
  // 서비스마다 두 줄: "Claude Code" / "5시간 43%   주간 12%" (최대 2개)
  out[0] = '\0';
  size_t used = 0;
  int lines = 0;
  for (JsonObjectConst svc : last_usage.as<JsonArrayConst>()) {
    JsonArrayConst bars = svc["bars"];
    if (bars.size() == 0 || lines >= 2) continue;
    used += snprintf(out + used, used < size ? size - used : 0, "%s%s\n", lines ? "\n" : "", svc["name"] | "");
    bool first = true;
    for (JsonObjectConst bar : bars) {
      used += snprintf(out + used, used < size ? size - used : 0, "%s%s %s", first ? "" : "   ", bar["label"] | "",
                       bar["text"] | "");
      first = false;
    }
    lines++;
  }
  if (used >= size) out[size - 1] = '\0';
}

void ui_set_mic(bool on) {
  // 기기 마이크(INMP441): 설정 > 알림 및 사운드, BOOT 버튼, PC(--mic) 로 켜고 끈다.
  // 사용량 화면 아래 버튼은 받아쓰기 버튼이다 (PC 마이크).
  mic_on = on;
  if (!on) ui_set_level(0);
}

void ui_set_level(uint8_t pct) {
  lv_bar_set_value(level_bar, pct, LV_ANIM_OFF);
}

void ui_show(const char *name) {
  // 설정 안의 어느 화면이든 그 화면 그대로 맨 아래로만 내린다
  if (strcmp(name, "settings_end") == 0 && view == VIEW_SETTINGS) {
    settings_open("end");
    return;
  }
  // 진단용: 사용량 화면의 첫 구간 기록 그래프. history_demo 는 화면에만 본보기 값을 그린다 (저장하지 않음).
  if (strcmp(name, "history") == 0 || strcmp(name, "history_demo") == 0) {
    view = VIEW_USAGE;
    apply_view();
    if (gauge_count == 0) return;
    hist_show(gauge_keys[0]);
    if (name[7] == '_') {
      for (int i = 0; i < HISTORY_SLOTS; i++) {
        // 5시간마다 0 으로 돌아가는 톱니 모양, 밤(가운데)은 비어 있음
        hist_points[i] = (i > 50 && i < 80) ? LV_CHART_POINT_NONE : (i % 30) * 3;
      }
      lv_chart_refresh(hist_chart);
    }
    return;
  }
  if (strcmp(name, "home") == 0) {
    view = VIEW_HOME;
  } else if (strcmp(name, "usage") == 0) {
    view = VIEW_USAGE;
  } else if (strcmp(name, "info") == 0) {
    // 정보는 설정 안으로 옮겼다
    view = VIEW_SETTINGS;
    apply_view();
    settings_open("info");
    return;
  } else if (strcmp(name, "shortcuts") == 0 || strcmp(name, "programs") == 0) {
    view = VIEW_SHORTCUTS;
    pcpanel_select_shortcuts(0);
  } else if (strcmp(name, "keywords") == 0) {
    view = VIEW_SHORTCUTS;
    pcpanel_select_shortcuts(1);
  } else if (strcmp(name, "pc") == 0) {
    // PC 상태는 사용량 화면의 마지막 페이지
    view = VIEW_USAGE;
    apply_view();
    lv_obj_set_tile_id(tiles, 0, pc_tile, LV_ANIM_OFF);
    update_pc_watch();
    return;
  } else if (strcmp(name, "ask") == 0) {
    view = VIEW_ASK;
  } else if (strcmp(name, "orca") == 0) {
    view = VIEW_ORCA;
  } else if (strncmp(name, "settings", 8) == 0) {
    view = VIEW_SETTINGS;
  }
  apply_view();
  // "settings_sd", "settings_bluetooth" 처럼 분류 화면을 바로 연다. "settings_end" 는 맨 아래로.
  if (strncmp(name, "settings_", 9) == 0) settings_open(name + 9);
}

bool ui_screenshot(uint16_t **pixels, int *w, int *h) {
  lv_img_dsc_t *snap = lv_snapshot_take(lv_scr_act(), LV_IMG_CF_TRUE_COLOR);
  if (!snap) return false;
  *w = snap->header.w;
  *h = snap->header.h;
  *pixels = (uint16_t *)ps_malloc(*w * *h * 2);
  if (*pixels) memcpy(*pixels, snap->data, *w * *h * 2);
  lv_snapshot_free(snap);
  if (!*pixels) return false;
  // 위에 덮이는 층(카메라 화면, 알림)도 보이는 것만 그 자리에 겹쳐 그린다
  for (lv_obj_t *layer : {lv_layer_top(), lv_layer_sys()}) {
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(layer); i++) {
      lv_obj_t *child = lv_obj_get_child(layer, i);
      if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
      lv_img_dsc_t *part = lv_snapshot_take(child, LV_IMG_CF_TRUE_COLOR);
      if (!part) continue;
      lv_area_t area;
      lv_obj_get_coords(child, &area);
      const uint16_t *src = (const uint16_t *)part->data;
      for (int y = 0; y < (int)part->header.h; y++) {
        int ty = area.y1 + y;
        if (ty < 0 || ty >= *h) continue;
        for (int x = 0; x < (int)part->header.w; x++) {
          int tx = area.x1 + x;
          if (tx >= 0 && tx < *w) (*pixels)[ty * *w + tx] = src[y * part->header.w + x];
        }
      }
      lv_snapshot_free(part);
    }
  }
  return true;
}

static lv_obj_t *toast;
static lv_timer_t *toast_timer;

static void toast_hide(lv_timer_t *t) {
  lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
  lv_timer_pause(t);
}

void ui_toast(const char *text, uint32_t ms) {
  if (!toast) {
    // 모든 화면(카메라 포함) 위에 뜨는 알림
    toast = lv_label_create(lv_layer_sys());
    lv_obj_set_style_text_font(toast, &font_ko_20, 0);
    lv_obj_set_style_text_color(toast, lv_color_white(), 0);
    lv_obj_set_style_text_align(toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x2A3038), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(toast, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(toast, 2, 0);
    lv_obj_set_style_radius(toast, 10, 0);
    lv_obj_set_style_pad_all(toast, 12, 0);
    toast_timer = lv_timer_create(toast_hide, ms, NULL);
  }
  lv_label_set_text(toast, text);
  lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(toast, LV_ALIGN_CENTER, 0, 0);
  lv_timer_set_period(toast_timer, ms);
  lv_timer_reset(toast_timer);
  lv_timer_resume(toast_timer);
  // 화면이 꺼져 있었으면 켜서 보이게 한다
  display_poke();
}
