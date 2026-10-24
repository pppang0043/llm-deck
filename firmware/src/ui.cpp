#include "ui.h"

#include <lvgl.h>
#include <time.h>

#include "psram_json.h"
#include "askpanel.h"
#include "display.h"
#include "history.h"
#include "mirror.h"
#include "orcapanel.h"
#include "pcpanel.h"
#include "settings.h"

// 한글이 들어 있는 글꼴 (src/fonts, 나눔고딕). 큰 숫자(48)는 기본 글꼴을 쓴다.
LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);
LV_FONT_DECLARE(font_ko_28);

// 상태줄 높이. 오른쪽 끝 돌리기 버튼을 누르기 쉽게 넉넉히 둔다.
#define STATUS_H 32
#define MIC_H 40
// 가로 화면에서는 마이크 버튼이 오른쪽에 세로로 놓인다 (작은 글씨로 여러 줄)
#define MIC_W 52
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
static lv_obj_t *back_btn;  // 상태줄 왼쪽 끝. 홈이 아닐 때만 보인다
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
static uint32_t usage_version = 0;  // last_usage 가 바뀔 때마다 하나씩 는다
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

// 왼쪽과 오른쪽에 글자가 하나씩 있는 한 줄. 왼쪽 글자를 돌려준다.
static lv_obj_t *add_row(lv_obj_t *parent, const char *left, const lv_font_t *left_font, lv_color_t left_color,
                         const char *right, const lv_font_t *right_font, lv_color_t right_color) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_t *lbl = add_label(row, left, left_font, left_color);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_align(add_label(row, right, right_font, right_color), LV_ALIGN_RIGHT_MID, 0, 0);
  return lbl;
}

// ---- 미러링 나눠 쓰기 버튼 ----
// 구독제 화면(원래 화면)과 기기 화면에 하나씩. 지워지면 자리를 비운다.
static lv_obj_t *split_btns[2];

static void split_btn_event(lv_event_t *e) {
  mirror_set_split(!mirror_split());
}

static void split_btn_deleted(lv_event_t *e) {
  for (lv_obj_t *&b : split_btns) {
    if (b == lv_event_get_target(e)) b = nullptr;
  }
}

static void update_split_btn(lv_obj_t *btn) {
  bool split = mirror_split();
  if (mirror_active()) {
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_set_style_bg_color(btn, split ? COLOR_ACCENT : COLOR_MIC_OFF, 0);
  lv_label_set_text(lv_obj_get_child(btn, 0), split ? "합치기" : "분리");
}

lv_obj_t *ui_split_button(lv_obj_t *parent) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, LV_SIZE_CONTENT, 24);
  lv_obj_set_style_pad_hor(btn, 8, 0);
  lv_obj_set_style_pad_ver(btn, 0, 0);
  lv_obj_set_style_radius(btn, 6, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_ext_click_area(btn, 8);  // 작은 버튼이라 누르는 자리는 조금 넓게
  lv_obj_center(add_label(btn, "", &font_ko_14, lv_color_white()));
  lv_obj_add_event_cb(btn, split_btn_event, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(btn, split_btn_deleted, LV_EVENT_DELETE, NULL);
  for (lv_obj_t *&b : split_btns) {
    if (!b) {
      b = btn;
      break;
    }
  }
  update_split_btn(btn);
  return btn;
}

void ui_mirror_changed() {
  for (lv_obj_t *b : split_btns) {
    if (b) update_split_btn(b);
  }
}

// 첫 사용량 페이지 제목 옆에 나눠 쓰기 버튼
static void add_title_split_btn(lv_obj_t *title) {
  lv_obj_t *btn = ui_split_button(lv_obj_get_parent(title));
  lv_obj_update_layout(title);  // 제목 폭을 알아야 바로 옆에 둔다
  lv_obj_align_to(btn, title, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
}

// 표: 첫 줄은 열 제목, 나머지는 [줄 이름, 값, 값, ...]. 여러 회사의 청구액을 한 화면에 나란히 보여준다
static void add_table(lv_obj_t *parent, JsonArrayConst table) {
  bool header = true;
  for (JsonArrayConst cells : table) {
    int n = cells.size();
    if (n == 0) continue;
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    // 줄 이름 칸은 30%, 나머지를 값 칸이 나눠 쓴다
    int other = n > 1 ? 70 / (n - 1) : 70;
    for (int i = 0; i < n; i++) {
      lv_color_t color = header ? COLOR_ACCENT : i == 0 ? COLOR_DIM : lv_color_white();
      lv_obj_t *cell = add_label(row, cells[i] | "", header || i == 0 ? &font_ko_14 : &font_ko_20, color);
      lv_label_set_long_mode(cell, LV_LABEL_LONG_CLIP);
      lv_obj_set_width(cell, lv_pct(i == 0 ? 30 : other));
      lv_obj_set_style_text_align(cell, i == 0 ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
    }
    header = false;
  }
}

// ---- 사용률 기록 그래프 (구간을 누르면 화면 전체에 최근 24시간, 오른쪽 위 버튼으로 7일) ----

struct GaugeKey {
  char service[32];
  char label[16];
};
#define MAX_GAUGES 8
static GaugeKey gauge_keys[MAX_GAUGES];
static int gauge_count = 0;
static bool no_history = false;  // 미러링 중 기기 화면에 그릴 때는 눌러도 그래프를 열지 않는다
static lv_obj_t *hist_panel, *hist_title, *hist_chart, *hist_note, *hist_left, *hist_right;
static lv_obj_t *hist_mode_btn, *hist_mode_lbl, *hist_days;  // 24시간/7일 버튼, 7일 막대 아래 요일
static lv_chart_series_t *hist_series;
static lv_coord_t hist_points[HISTORY_SLOTS];
static GaugeKey hist_key;
static bool hist_week_mode = false;
static const char *const WDAY_NAMES[7] = {"일", "월", "화", "수", "목", "금", "토"};

static void hist_close(lv_event_t *e) {
  lv_obj_add_flag(hist_panel, LV_OBJ_FLAG_HIDDEN);
}

static void hist_render();

static void hist_mode_clicked(lv_event_t *e) {
  hist_week_mode = !hist_week_mode;
  hist_render();
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

  // 오른쪽 위: 누르면 24시간 <-> 7일 (닫히지 않는다)
  hist_mode_btn = lv_btn_create(hist_panel);
  lv_obj_set_style_bg_color(hist_mode_btn, lv_color_hex(0x1C232B), 0);
  lv_obj_set_style_border_color(hist_mode_btn, COLOR_ACCENT, 0);
  lv_obj_set_style_border_width(hist_mode_btn, 1, 0);
  lv_obj_set_style_shadow_width(hist_mode_btn, 0, 0);
  lv_obj_set_style_radius(hist_mode_btn, 12, 0);
  lv_obj_set_style_pad_hor(hist_mode_btn, 8, 0);
  lv_obj_set_style_pad_ver(hist_mode_btn, 3, 0);
  lv_obj_set_ext_click_area(hist_mode_btn, 8);
  lv_obj_align(hist_mode_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_add_event_cb(hist_mode_btn, hist_mode_clicked, LV_EVENT_CLICKED, NULL);
  hist_mode_lbl = add_label(hist_mode_btn, "7일", &font_ko_14, COLOR_ACCENT);
  lv_obj_center(hist_mode_lbl);

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
  lv_obj_set_style_pad_all(hist_chart, 6, 0);
  lv_obj_set_style_pad_column(hist_chart, 6, 0);  // 7일 막대 사이
  hist_series = lv_chart_add_series(hist_chart, COLOR_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
  // 그래프 아래 양 끝: 시간 축
  hist_left = add_label(hist_panel, "24시간 전", &font_ko_14, COLOR_DIM);
  hist_right = add_label(hist_panel, "지금", &font_ko_14, COLOR_DIM);
  lv_chart_set_ext_y_array(hist_chart, hist_series, hist_points);

  // 7일 막대 아래: 막대마다 요일 (막대와 같은 간격으로 나눈다)
  hist_days = lv_obj_create(hist_panel);
  lv_obj_remove_style_all(hist_days);
  lv_obj_clear_flag(hist_days, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(hist_days, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_set_flex_flow(hist_days, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_hor(hist_days, 6, 0);
  lv_obj_set_style_pad_column(hist_days, 6, 0);
  for (int i = 0; i < 7; i++) {
    lv_obj_t *d = add_label(hist_days, "", &font_ko_14, COLOR_DIM);
    lv_obj_set_flex_grow(d, 1);
    lv_obj_set_width(d, 1);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
  }
}

// 24시간: 사용률(%) 선 그래프
static void hist_fill_day(char *note, size_t size) {
  uint8_t v[HISTORY_SLOTS];
  bool have = history_get(hist_key.service, hist_key.label, v);
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
  if (known == 0) {
    snprintf(note, size, "아직 기록이 없습니다 (10분마다 모읍니다).\n누르면 닫힘");
  } else {
    snprintf(note, size, "최고 %d%%, 지금 %d%%. 누르면 닫힘", peak, last);
  }
}

// 7일: 날마다 오른 % 의 합을 막대로 (맨 오른쪽이 오늘), 아래에 가장 많이 쓴 날과 시간대
static void hist_fill_week(char *note, size_t size) {
  static uint8_t h[WEEK_HOURS];
  bool have = history_week(hist_key.service, hist_key.label, h);
  if (!have) memset(h, HISTORY_NONE, sizeof(h));
  WeekSummary s = history_summarize(h, time(nullptr));
  int top = 10;
  for (int d = 0; d < 7; d++) {
    hist_points[d] = s.day[d];
    if (s.day[d] > top) top = s.day[d];
  }
  lv_chart_set_range(hist_chart, LV_CHART_AXIS_PRIMARY_Y, 0, (top + 9) / 10 * 10);
  for (int d = 0; d < 7; d++) {
    lv_obj_t *lbl = lv_obj_get_child(hist_days, d);
    // 칸이 좁아 요일 한 글자만. 가장 많이 쓴 날은 주황, 오늘은 흰색.
    lv_label_set_text(lbl, WDAY_NAMES[s.day_wday[d]]);
    lv_color_t color = d == s.best_day ? COLOR_ACCENT : d == 6 ? lv_color_white() : COLOR_DIM;
    lv_obj_set_style_text_color(lbl, color, 0);
  }
  if (s.best_day < 0) {
    snprintf(note, size, "최근 7일 쓴 기록이 아직 없습니다.\n막대는 하루 동안 오른 %%. 누르면 닫힘");
    return;
  }
  snprintf(note, size, "가장 많이 쓴 날: %s요일 (+%d%%)\n가장 많이 쓴 시간대: %d~%d시\n7일 동안 +%d%%. 누르면 닫힘",
           WDAY_NAMES[s.day_wday[s.best_day]], s.day[s.best_day], s.best_hour, (s.best_hour + 2) % 24, s.total);
}

static void hist_render() {
  char note[160];
  if (hist_week_mode) {
    lv_chart_set_type(hist_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(hist_chart, 7);
    lv_chart_set_div_line_count(hist_chart, 3, 0);
    hist_fill_week(note, sizeof(note));
  } else {
    lv_chart_set_type(hist_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(hist_chart, HISTORY_SLOTS);
    lv_chart_set_range(hist_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(hist_chart, 5, 0);  // 25% 마다 가로줄
    hist_fill_day(note, sizeof(note));
  }
  lv_label_set_text(hist_note, note);
  lv_label_set_text(hist_mode_lbl, hist_week_mode ? "24시간" : "7일");
  lv_obj_update_layout(hist_mode_btn);
  int btn_w = lv_obj_get_width(hist_mode_btn);
  lv_label_set_text_fmt(hist_title, "%s %s", hist_key.service, hist_key.label);
  lv_label_set_long_mode(hist_title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(hist_title, display_width() - 20 - btn_w - 6);
  // 화면 방향에 맞춰 크기를 다시 정한다. 그래프 아래는 축(요일) 글자 한 줄 + 쪽지 줄 수만큼 (한 줄 17px)
  int lines = 1;
  for (const char *p = note; *p; p++) lines += *p == '\n';
  int w = display_width(), hgt = display_height();
  lv_obj_set_width(hist_note, w - 20);
  lv_obj_set_size(hist_panel, w, hgt);
  int below = 25 + 17 * lines;
  lv_obj_set_size(hist_chart, lv_pct(100), hgt - 20 - 30 - below);
  lv_obj_align(hist_chart, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_align_to(hist_left, hist_chart, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);
  lv_obj_align_to(hist_right, hist_chart, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
  lv_obj_set_size(hist_days, w - 20, LV_SIZE_CONTENT);
  lv_obj_align_to(hist_days, hist_chart, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
  if (hist_week_mode) {
    lv_obj_add_flag(hist_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hist_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(hist_days, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(hist_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(hist_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hist_days, LV_OBJ_FLAG_HIDDEN);
  }
  lv_chart_refresh(hist_chart);
}

static void hist_show(const GaugeKey &k, bool week = false) {
  if (!hist_panel) hist_build();
  hist_key = k;
  hist_week_mode = week;
  hist_render();
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

// ---- 소진 예상: 최근 사용률 기록으로 지금 속도면 언제 100% 가 되는지 ----

// 초기화 전에 한도가 찰 것 같으면 "16:40 소진 예상" (오늘이 아니면 "23일 16시 소진 예상") 을 out 에 넣고 true.
// 계산은 history_forecast (한도 알림의 미리 알림도 같은 값을 쓴다).
static bool forecast(const char *service, JsonObjectConst bar, char *out, size_t size) {
  time_t full = history_forecast(service, bar["label"] | "", bar["pct"] | 0, bar["resets"] | 0u);
  if (!full) return false;
  time_t now = time(nullptr);
  struct tm t, today;
  localtime_r(&full, &t);
  localtime_r(&now, &today);
  if (t.tm_yday == today.tm_yday && t.tm_year == today.tm_year) {
    snprintf(out, size, "%02d:%02d 소진 예상", t.tm_hour, t.tm_min);
  } else {
    snprintf(out, size, "%d일 %d시 소진 예상", t.tm_mday, t.tm_hour);
  }
  return true;
}

// 소진 예상 글이 칸에 다 들어가지 않으면 끝의 " 예상" 을 뺀다
static void fit_forecast(lv_obj_t *lbl) {
  static const char TAIL[] = " 예상";
  const size_t tail = sizeof(TAIL) - 1;
  lv_obj_t *parent = lv_obj_get_parent(lbl);
  lv_obj_update_layout(parent);
  lv_coord_t room = lv_obj_get_content_width(parent);
  const char *text = lv_label_get_text(lbl);
  size_t len = strlen(text);
  lv_point_t size;
  lv_txt_get_size(&size, text, lv_obj_get_style_text_font(lbl, LV_PART_MAIN), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  if (room <= 0 || size.x <= room || len <= tail || strcmp(text + len - tail, TAIL) != 0) return;
  char shorter[32];
  strlcpy(shorter, text, LV_MIN(sizeof(shorter), len - tail + 1));
  lv_label_set_text(lbl, shorter);
}

// ---- 리셋까지 남은 시간: 페이지를 다시 만들지 않고 30초마다 글자만 바꾼다 ----

struct Remain {
  lv_obj_t *label;
  time_t resets;
};
#define MAX_REMAINS 12
static Remain remains[MAX_REMAINS];
static lv_timer_t *remain_timer;

// 한 줄로 "2:13 (15:00)" (하루가 넘으면 "6일 2:13 (30일(금) 15:00)").
// 칸에 다 들어가지 않으면 시:분, 괄호 순으로 덜어 낸다 (줄을 바꾸거나 "..." 로 자르지 않는다).
static void remain_update(Remain &r) {
  time_t now = time(nullptr);
  long left = (long)(r.resets - now);
  if (left < 0) left = 0;
  long days = left / 86400, hours = left % 86400 / 3600, mins = left % 3600 / 60;
  struct tm t, today;
  localtime_r(&r.resets, &t);
  localtime_r(&now, &today);
  char when[32], clock[16], day[16];
  if (t.tm_yday == today.tm_yday && t.tm_year == today.tm_year) {
    snprintf(when, sizeof(when), "%02d:%02d", t.tm_hour, t.tm_min);
  } else {
    snprintf(when, sizeof(when), "%d일(%s) %02d:%02d", t.tm_mday, WDAY_NAMES[t.tm_wday], t.tm_hour, t.tm_min);
  }
  snprintf(clock, sizeof(clock), "%ld:%02ld", hours, mins);
  snprintf(day, sizeof(day), "%ld일", days);
  char forms[3][48];
  int n = 0;
  if (days) {
    snprintf(forms[n++], 48, "%s %s (%s)", day, clock, when);
    snprintf(forms[n++], 48, "%s (%s)", day, when);
    snprintf(forms[n++], 48, "%s %s", day, clock);
  } else {
    snprintf(forms[n++], 48, "%s (%s)", clock, when);
    snprintf(forms[n++], 48, "%s", clock);
  }
  lv_obj_t *parent = lv_obj_get_parent(r.label);
  lv_obj_update_layout(parent);
  lv_coord_t room = lv_obj_get_content_width(parent);
  const lv_font_t *font = lv_obj_get_style_text_font(r.label, LV_PART_MAIN);
  const char *text = forms[n - 1];
  for (int i = 0; i < n; i++) {
    lv_point_t size;
    lv_txt_get_size(&size, forms[i], font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (room <= 0 || size.x <= room) {
      text = forms[i];
      break;
    }
  }
  if (strcmp(lv_label_get_text(r.label), text) != 0) lv_label_set_text(r.label, text);
}

static void remain_tick(lv_timer_t *t) {
  for (Remain &r : remains) {
    if (r.label) remain_update(r);
  }
}

static void remain_deleted(lv_event_t *e) {
  for (Remain &r : remains) {
    if (r.label == lv_event_get_target(e)) r.label = nullptr;
  }
}

// label 에 남은 시간을 쓰고 계속 고쳐 나간다. 초기화 시각을 모르면 아무것도 하지 않는다.
// 처음 글자는 페이지 배치가 끝난 뒤(칸 폭을 알 때) 정한다.
static void add_remain(lv_obj_t *label, uint32_t resets) {
  if (!resets) return;
  for (Remain &r : remains) {
    if (r.label) continue;
    r.label = label;
    r.resets = (time_t)resets;
    lv_obj_add_event_cb(label, remain_deleted, LV_EVENT_DELETE, NULL);
    if (!remain_timer) remain_timer = lv_timer_create(remain_tick, 30000, NULL);
    lv_async_call([](void *) { remain_tick(nullptr); }, nullptr);
    return;
  }
}

// 한도 한 구간: 왼쪽에 이름과 초기화 시각, 오른쪽에 큰 숫자, 아래에 막대. 누르면 기록 그래프.
static void add_gauge(lv_obj_t *parent, const char *service, JsonObjectConst bar) {
  int pct = constrain(bar["pct"] | 0, 0, 100);
  lv_color_t color = pct >= 90 ? COLOR_FULL : pct >= 70 ? COLOR_WARN : lv_color_white();

  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_align(add_label(row, bar["label"] | "", &font_ko_28, COLOR_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
  // 리셋 시각을 알면 남은 시간과 함께 밝게 ("0일 02:13 (15:00)")
  bool known = bar["resets"] | 0u;
  lv_obj_t *note = add_label(row, bar["note"] | "", known ? &font_ko_20 : &font_ko_14,
                             known ? lv_color_white() : COLOR_DIM);
  lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  add_remain(note, bar["resets"] | 0u);
  lv_obj_align(add_label(row, bar["text"] | "", &lv_font_montserrat_48, color), LV_ALIGN_RIGHT_MID, 0, 0);
  if (!no_history && gauge_count < MAX_GAUGES) {
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
  char soon[32];
  if (forecast(service, bar, soon, sizeof(soon))) add_label(parent, soon, &font_ko_14, COLOR_WARN);
}

// 한도 막대만 있는 페이지(Claude Code, Codex 같은 구독)인지
static bool is_limits_page(JsonObjectConst svc) {
  return svc["bars"].as<JsonArrayConst>().size() > 0 && !svc["headline"].is<const char *>() &&
         !svc["table"].is<JsonArrayConst>() && !svc["rows"].is<JsonArrayConst>();
}

// 표의 소진 예상 글. 칸 폭은 같은 줄의 칸이 다 생겨야 정해지므로 표를 다 만든 뒤 fit_forecast 로 맞춘다
static lv_obj_t *table_forecasts[8];
static int table_forecast_count;

// 구독 한도 표의 한 칸: 큰 숫자, 막대, 초기화 시각. 누르면 기록 그래프.
static void add_limit_cell(lv_obj_t *row, const char *service, JsonObjectConst bar, int width_pct) {
  lv_obj_t *cell = lv_obj_create(row);
  lv_obj_remove_style_all(cell);
  lv_obj_set_size(cell, lv_pct(width_pct), LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(cell, 1);  // 이름 칸을 뺀 나머지를 서비스가 나눠 쓴다
  lv_obj_set_style_pad_left(cell, 6, 0);
  lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_row(cell, 3, 0);
  if (bar.isNull()) return;

  int pct = constrain(bar["pct"] | 0, 0, 100);
  lv_color_t color = pct >= 90 ? COLOR_FULL : pct >= 70 ? COLOR_WARN : lv_color_white();
  add_label(cell, bar["text"] | "", &lv_font_montserrat_28, color);
  lv_obj_t *b = lv_bar_create(cell);
  lv_obj_set_size(b, lv_pct(100), 8);
  lv_bar_set_range(b, 0, 100);
  lv_bar_set_value(b, pct, LV_ANIM_OFF);
  if (pct >= 70) lv_obj_set_style_bg_color(b, color, LV_PART_INDICATOR);
  // 리셋 시각을 알면 남은 시간과 함께 밝게 ("0일 02:13 (15:00)"), 모르면 받은 글자를 흐리게
  const char *when = bar["note"] | "";
  bool known = bar["resets"] | 0u;
  lv_obj_t *note = add_label(cell, when[0] ? when : "-", &font_ko_14, known ? lv_color_white() : COLOR_DIM);
  lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);
  lv_obj_set_style_max_width(note, lv_pct(100), 0);
  add_remain(note, bar["resets"] | 0u);  // 칸 폭에 맞는 한 줄을 고른다
  char soon[32];
  if (forecast(service, bar, soon, sizeof(soon))) {
    lv_obj_t *lbl = add_label(cell, soon, &font_ko_14, COLOR_WARN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(lbl, lv_pct(100), 0);
    if (table_forecast_count < 8) table_forecasts[table_forecast_count++] = lbl;
  }
  if (!no_history && gauge_count < MAX_GAUGES) {
    GaugeKey &k = gauge_keys[gauge_count];
    strlcpy(k.service, service, sizeof(k.service));
    strlcpy(k.label, bar["label"] | "", sizeof(k.label));
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cell, gauge_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)gauge_count);
    gauge_count++;
  }
}

// 구독 한도 페이지 여럿을 한 페이지에 표로: 열은 서비스, 줄은 구간(5시간, 주간).
// 제목 줄은 두지 않고(스크롤 없이 한 화면에 들어가게) 받아 온 시각(note)을 서비스 이름 줄 왼쪽 빈칸에 둔다.
// split 이면 미러링 나눠 쓰기 버튼을 그 칸에 겹쳐 둔다 (미러링 중에만 보이고 그동안 시각은 가린다).
static void add_limits_table(lv_obj_t *page, JsonArrayConst services, const char *note, bool split) {
  table_forecast_count = 0;
  JsonObjectConst list[4];
  int n = 0;
  for (JsonObjectConst svc : services) {
    if (is_limits_page(svc) && n < 4) list[n++] = svc;
  }
  int other = 76 / n;  // flex_grow 로 늘어나기 전의 칸 폭
  lv_obj_t *head = lv_obj_create(page);
  lv_obj_remove_style_all(head);
  lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
  // 서비스 이름은 오른쪽 끝을 아래 칸에 맞추고 왼쪽으로 조금 더 써서 세로 화면에서도 한 줄에 ("Claude Code")
  int head_w = 80 / n;
  lv_obj_t *when = add_label(head, note, &font_ko_14, COLOR_DIM);
  lv_obj_set_width(when, lv_pct(100 - head_w * n));
  lv_label_set_long_mode(when, LV_LABEL_LONG_CLIP);
  // 오늘이 아니면 "10월 23일 15:18" 로 와서 칸에 다 들어가지 않는다: 그때는 날짜("23일")만
  lv_obj_update_layout(head);
  lv_point_t size;
  lv_txt_get_size(&size, note, &font_ko_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  const char *day = strchr(note, ' ');
  if (size.x > lv_obj_get_content_width(when) && day) {
    char shorter[16];
    strlcpy(shorter, day + 1, sizeof(shorter));
    if (char *end = strchr(shorter, ' ')) *end = '\0';
    lv_label_set_text(when, shorter);
  }
  if (split) {
    lv_obj_add_flag(head, LV_OBJ_FLAG_OVERFLOW_VISIBLE);  // 버튼(24px)이 이름 줄보다 높다
    lv_obj_t *btn = ui_split_button(head);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);  // 이름 줄 높이에 넣지 않는다
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
  }
  for (int i = 0; i < n; i++) {
    lv_obj_t *name = add_label(head, list[i]["name"] | "?", &font_ko_14, COLOR_ACCENT);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, lv_pct(head_w));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_RIGHT, 0);
  }
  // 구간 이름은 첫 서비스 순서대로, 다른 서비스에만 있는 구간은 뒤에
  const char *labels[6];
  int nl = 0;
  for (int i = 0; i < n; i++) {
    for (JsonObjectConst bar : list[i]["bars"].as<JsonArrayConst>()) {
      const char *label = bar["label"] | "";
      bool seen = false;
      for (int j = 0; j < nl; j++) seen |= strcmp(labels[j], label) == 0;
      if (!seen && nl < 6) labels[nl++] = label;
    }
  }
  // 이름 칸은 가장 긴 구간 이름("5시간")만큼만: 남는 폭은 리셋 글자가 한 줄에 들어가도록 칸에 준다
  lv_coord_t name_w = 0;
  for (int j = 0; j < nl; j++) {
    lv_point_t size;
    lv_txt_get_size(&size, labels[j], &font_ko_20, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    name_w = LV_MAX(name_w, size.x + 2);
  }
  for (int j = 0; j < nl; j++) {
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *name = add_label(row, labels[j], &font_ko_20, COLOR_DIM);
    lv_obj_set_width(name, name_w);
    lv_obj_set_style_pad_top(name, 6, 0);
    for (int i = 0; i < n; i++) {
      JsonObjectConst found;
      for (JsonObjectConst bar : list[i]["bars"].as<JsonArrayConst>()) {
        if (strcmp(bar["label"] | "", labels[j]) == 0) found = bar;
      }
      add_limit_cell(row, list[i]["name"] | "?", found, other);
    }
  }
  for (int i = 0; i < table_forecast_count; i++) fit_forecast(table_forecasts[i]);
}

static lv_obj_t *add_page(lv_obj_t *tv, uint8_t index, uint8_t col = 0) {
  // 서비스 페이지는 위아래로 넘긴다 (오른쪽으로 밀기는 홈으로 가기에 쓴다). 옆 페이지(col 1)는 왼쪽으로만 돌아간다.
  lv_obj_t *page = lv_tileview_add_tile(tv, col, index, col ? LV_DIR_LEFT : LV_DIR_VER);
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
  lv_obj_t *page = add_page(tiles, 0);
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
  // 가로 화면의 버튼은 좁아서 작은 글씨로 여러 줄로 쓴다
  lv_obj_set_style_text_font(mic_lbl, wide ? &font_ko_14 : &font_ko_20, 0);
  const char *text;
  int step = askpanel_dictate_step();
  if (st == ASK_TALKING && step == DICTATE_PREPARING) {
    text = wide ? "마이크\n준비\n중..." : "마이크 준비 중...";
  } else if (st == ASK_TALKING) {
    text = wide ? LV_SYMBOL_AUDIO "\n말하\n세요\n\n다시\n누르면\n입력" : LV_SYMBOL_AUDIO "  말하세요... 다시 누르면 입력";
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

// 뒤로 가기: 설정의 분류 화면에서는 먼저 목록으로 돌아가고 (아이폰의 뒤로 가기처럼), 나머지는 홈으로
static void go_back() {
  if (view == VIEW_HOME) return;
  if (view == VIEW_SETTINGS && settings_back()) return;
  ui_show_home();
}

// 사용량 페이지는 화면의 1/5 만 끌어도 넘어간다 (tileview 는 절반을 넘겨야 넘어가 손을 많이 움직여야 했다).
// 손을 떼면 tileview 가 가까운 페이지로 되돌아가는 움직임을 시작하는데, 그때 목적지를 끌던 쪽 옆 페이지로 바꾼다.
static void easy_paging(lv_event_t *e) {
  lv_anim_t *a = (lv_anim_t *)lv_event_get_param(e);  // 손으로 끄는 중이면 NULL
  lv_obj_t *tv = lv_event_get_target(e);
  lv_obj_t *active = lv_tileview_get_tile_act(tv);
  if (!a || !active) return;
  lv_coord_t w = lv_obj_get_content_width(tv), h = lv_obj_get_content_height(tv);
  lv_coord_t tx = lv_obj_get_x(active), ty = lv_obj_get_y(active);
  lv_coord_t dx = lv_obj_get_scroll_x(tv) - tx, dy = lv_obj_get_scroll_y(tv) - ty;
  bool vertical = dy != 0;
  lv_coord_t moved = vertical ? dy : dx, size = vertical ? h : w;
  // 지금 페이지로 되돌아가려는 것만 바꾼다 (이미 옆 페이지로 가는 중이면 그대로)
  if (-a->end_value != (vertical ? ty : tx) || moved == 0 || LV_ABS(moved) * 5 < size) return;
  lv_coord_t nx = vertical ? tx : tx + (moved > 0 ? w : -w), ny = vertical ? ty + (moved > 0 ? h : -h) : ty;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(tv); i++) {
    lv_obj_t *tile = lv_obj_get_child(tv, i);
    if (lv_obj_get_x(tile) == nx && lv_obj_get_y(tile) == ny) {
      a->end_value = -(vertical ? ny : nx);
      return;
    }
  }
}

static void screen_gesture(lv_event_t *e) {
  // 어디서든 오른쪽으로 밀면 뒤로
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) != LV_DIR_RIGHT || view == VIEW_HOME) return;
  go_back();
  // LVGL 은 밀고 난 뒤 손을 떼도 처음 누른 버튼에 "눌림" 을 보낸다. 버튼 위에서 밀기 시작하면
  // 뒤로 가면서 그 버튼(키워드 입력 등)까지 눌렸다. 손을 뗄 때까지 아무것도 누르지 않게 한다
  lv_indev_wait_release(lv_indev_get_act());
}

// 상태줄 왼쪽: [뒤로 버튼] [연결 방식]. 뒤로 버튼이 숨으면 글자가 왼쪽 끝으로 간다.
static void layout_status_right();
static void layout_status_left() {
  bool back = view != VIEW_HOME;
  if (back) {
    lv_obj_clear_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_align(status_lbl, LV_ALIGN_TOP_LEFT, 2 + 40 + 6, (STATUS_H - 16) / 2);
  } else {
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(status_lbl, LV_ALIGN_TOP_LEFT, 8, (STATUS_H - 16) / 2);
  }
  layout_status_right();
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

// 홈 아이콘 배치: 가로 화면 3x2, 세로 화면 2x3. 가로 화면에서는 카메라와 ORCA 자리를 바꾼다.
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
    int slot = cols == 3 && (i == 1 || i == 4) ? 5 - i : i;
    lv_obj_set_size(home_icons[i], cw, 82);
    lv_obj_set_pos(home_icons[i], (slot % cols) * cw, (slot / cols) * ch + (ch - 82) / 2);
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
  layout_status_left();
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

  // 뒤로 버튼: 화면 돌리기 버튼과 같은 모양으로 상태줄 왼쪽 끝에 (오른쪽으로 밀기와 같다)
  back_btn = lv_btn_create(scr);
  lv_obj_set_size(back_btn, 40, STATUS_H - 4);
  lv_obj_set_style_bg_color(back_btn, COLOR_MIC_OFF, 0);
  lv_obj_set_style_shadow_width(back_btn, 0, 0);
  lv_obj_set_style_pad_all(back_btn, 0, 0);
  lv_obj_set_ext_click_area(back_btn, 8);
  lv_obj_add_event_cb(back_btn, [](lv_event_t *e) { go_back(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_center(add_label(back_btn, LV_SYMBOL_LEFT, &lv_font_montserrat_20, lv_color_white()));

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
  lv_obj_add_event_cb(tiles, easy_paging, LV_EVENT_SCROLL_BEGIN, NULL);
  lv_obj_add_event_cb(tiles, [](lv_event_t *e) { update_pc_watch(); }, LV_EVENT_VALUE_CHANGED, NULL);

  level_bar = lv_bar_create(scr);
  lv_obj_set_style_radius(level_bar, 0, 0);
  lv_obj_set_style_radius(level_bar, 0, LV_PART_INDICATOR);
  lv_bar_set_range(level_bar, 0, 100);
  // 기기 마이크(INMP441)의 소리 크기. 지금은 마이크를 달지 않아 쓰지 않는다.
  lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);

  mic_btn = lv_btn_create(scr);
  lv_obj_set_style_radius(mic_btn, 0, 0);
  lv_obj_set_style_pad_all(mic_btn, 0, 0);
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
  // 열려 있는 기록 그래프도 새 방향에 맞춘다
  if (ui_history_visible()) hist_render();
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

// 누르고 있거나 페이지가 넘어가는 중인지. 그동안 페이지를 새로 만들면 누르던 버튼과 넘기던 동작이 사라진다.
static bool touch_busy() {
  lv_indev_t *indev = lv_indev_get_next(NULL);
  return (indev && indev->proc.state == LV_INDEV_STATE_PRESSED) || lv_anim_get(tiles, NULL);
}

// 새로 온 사용량은 손을 뗀 뒤에 그린다
static lv_timer_t *usage_timer;

static void usage_retry(lv_timer_t *t) {
  if (touch_busy()) return;
  lv_timer_pause(t);
  render_usage();
}

void ui_set_usage(JsonArrayConst services) {
  history_record(services);
  // PC 도우미는 30초마다 같은 내용을 다시 보낸다. 바뀐 것이 없으면 그대로 둔다 (새로 만드는 데 0.1초쯤 멈춘다).
  // 다만 "16:40 소진 예상" 과 오늘/날짜 표시는 시간이 지나면 바뀌므로 기록 한 칸(10분)마다 다시 그린다.
  static long drawn_slot = -1;
  long slot = (long)(time(nullptr) / HISTORY_SLOT_S);
  if (!waiting && last_usage.as<JsonVariantConst>() == services && slot == drawn_slot) return;
  drawn_slot = slot;
  waiting = false;
  last_usage.set(services);
  usage_version++;
  if (!touch_busy()) {
    render_usage();
    return;
  }
  if (!usage_timer) usage_timer = lv_timer_create(usage_retry, 100, NULL);
  lv_timer_resume(usage_timer);
}

// 서비스 페이지들을 tv 에 만든다. 페이지 수(줄)를 돌려주고, 옆 페이지가 있는 줄을 side_rows 에 표시한다.
// 원래 화면이면 첫 페이지 제목 옆에 나눠 쓰기 버튼을 둔다 (기기 화면은 위쪽 줄에 따로 있다).
static uint8_t build_usage_pages(lv_obj_t *tv, JsonArrayConst services, bool main_screen, uint32_t *side_rows) {
  uint8_t index = 0;
  // 구독 한도 페이지가 둘 이상이면 처음 것 자리에 "구독제" 한 페이지로 모은다
  int limits = 0;
  for (JsonObjectConst svc : services) limits += is_limits_page(svc);
  bool limits_done = false;
  lv_obj_t *prev = nullptr;  // 바로 앞 페이지 (옆 페이지가 오면 오른쪽으로도 넘기게 한다)
  *side_rows = 0;
  for (JsonObjectConst svc : services) {
    if (limits >= 2 && is_limits_page(svc)) {
      if (limits_done) continue;
      limits_done = true;
      bool first = index == 0;
      lv_obj_t *page = prev = add_page(tv, index++);
      add_limits_table(page, services, svc["note"] | "", first && main_screen);
      continue;
    }
    // 옆 페이지: 앞 페이지 오른쪽에 둔다
    bool side = svc["side"] == true && prev && index > 0 && !(*side_rows & (1u << (index - 1)));
    lv_obj_t *page;
    if (side) {
      ((lv_tileview_tile_t *)prev)->dir = (lv_dir_t)(LV_DIR_VER | LV_DIR_RIGHT);
      *side_rows |= 1u << (index - 1);
      page = add_page(tv, index - 1, 1);
    } else {
      page = prev = add_page(tv, index);
    }

    // 제목 오른쪽의 작은 글자는 이 값이 언제 것인지(시각)를 나타낸다
    lv_obj_t *title = add_row(page, svc["name"] | "?", &font_ko_28, COLOR_ACCENT, svc["note"] | "", &font_ko_14,
                              COLOR_DIM);
    if (index == 0 && !side && main_screen) add_title_split_btn(title);

    if (svc["headline"].is<const char *>()) {
      add_label(page, svc["headline"], &lv_font_montserrat_48, lv_color_white());
    }
    // 표가 있으면 설명(경고 등)은 표 아래에 둔다
    if (svc["table"].is<JsonArrayConst>()) {
      add_table(page, svc["table"]);
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
    if (!side) index++;
  }
  return index;
}

// 보고 있던 페이지 (줄, 칸). 다시 만들어도 그 자리에 머물게 한다.
static void tile_pos(lv_obj_t *tv, uint8_t *row, uint8_t *col) {
  lv_obj_t *active = lv_tileview_get_tile_act(tv);
  int tile_h = lv_obj_get_height(tv), tile_w = lv_obj_get_width(tv);
  *row = active && tile_h > 0 ? lv_obj_get_y(active) / tile_h : 0;
  *col = active && tile_w > 0 ? lv_obj_get_x(active) / tile_w : 0;
}

static void render_usage() {
  if (usage_timer) lv_timer_pause(usage_timer);
  JsonArrayConst services = last_usage.as<JsonArrayConst>();

  // 데이터가 갱신돼도 보고 있던 페이지에 머물도록 현재 위치를 기억해 둔다
  uint8_t keep, keep_col;
  tile_pos(tiles, &keep, &keep_col);
  // PC 상태를 보고 있었으면 페이지 수가 바뀌어도 PC 상태(맨 끝)에 머문다
  bool keep_pc = keep == pc_tile;

  size_t count = services.size();
  if (count == 0) {
    show_placeholder("표시할 내용 없음", "PC 도우미가 보낸 사용량이 없습니다.");
    return;
  }

  pcpanel_status_detach();
  lv_obj_clean(tiles);
  gauge_count = 0;
  uint32_t side_rows;
  uint8_t index = build_usage_pages(tiles, services, true, &side_rows);
  add_pc_tile(index);

  uint8_t row = keep_pc || keep > index ? index : keep;
  lv_obj_set_tile_id(tiles, keep_col && (side_rows & (1u << row)) ? 1 : 0, row, LV_ANIM_OFF);
  update_pc_watch();
}

// PC 상태 페이지가 보이는 동안만 PC 에 3초마다 보내 달라고 한다
static void update_pc_watch() {
  lv_obj_t *active = lv_tileview_get_tile_act(tiles);
  int tile_h = lv_obj_get_height(tiles);
  bool on_pc = active && tile_h > 0 && lv_obj_get_y(active) / tile_h == pc_tile;
  pcpanel_show_status(view == VIEW_USAGE && on_pc);
}

uint32_t ui_usage_version() {
  return usage_version;
}

void ui_usage_pages(lv_obj_t *tv) {
  JsonArrayConst services = last_usage.as<JsonArrayConst>();
  uint8_t keep, keep_col;
  tile_pos(tv, &keep, &keep_col);
  lv_obj_clean(tv);
  if (services.size() == 0) {
    lv_obj_t *page = add_page(tv, 0);
    lv_obj_t *lbl = add_label(page, "PC 도우미가 보낸 사용량이 없습니다.", &font_ko_20, COLOR_DIM);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    return;
  }
  // 그래프는 원래 화면에 열리므로 기기 화면에서는 눌러도 열지 않는다
  no_history = true;
  uint32_t side_rows;
  uint8_t count = build_usage_pages(tv, services, false, &side_rows);
  no_history = false;
  // 맨 끝은 원래 화면처럼 PC 상태
  lv_obj_t *tile = lv_tileview_add_tile(tv, 0, count, LV_DIR_VER);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  pcpanel_device_status(tile, lv_obj_get_width(tv), lv_obj_get_height(tv));
  uint8_t row = keep > count ? count : keep;
  lv_obj_set_tile_id(tv, keep_col && (side_rows & (1u << row)) ? 1 : 0, row, LV_ANIM_OFF);
}

bool ui_usage_pages_on_pc(lv_obj_t *tv) {
  lv_obj_t *active = lv_tileview_get_tile_act(tv);
  uint32_t n = lv_obj_get_child_cnt(tv);
  return active && n && active == lv_obj_get_child(tv, n - 1);
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
  // 진단용: 사용량 화면의 첫 구간 7일 그래프
  if (strcmp(name, "history_week") == 0) {
    view = VIEW_USAGE;
    apply_view();
    if (gauge_count) hist_show(gauge_keys[0], true);
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
  } else if (strcmp(name, "remote") == 0) {
    view = VIEW_SHORTCUTS;
    pcpanel_select_shortcuts(2);
  } else if (strncmp(name, "usage_", 6) == 0) {
    // 진단용: 사용량 화면의 N 번째 페이지 (0 부터)
    view = VIEW_USAGE;
    apply_view();
    lv_obj_set_tile_id(tiles, 0, atoi(name + 6), LV_ANIM_OFF);
    update_pc_watch();
    return;
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

// 지나간 알림 (설정 > 알림 기록). PSRAM 에 돌려 쓴다.
#define TOAST_LOG 30
struct ToastLog {
  time_t at;       // 시계를 못 맞췄으면 0
  uint32_t ms;     // 켠 뒤 시간
  char text[96];
};
static ToastLog *toast_log = nullptr;
static int toast_log_head = 0, toast_log_count = 0;

static void log_toast(const char *text) {
  if (!toast_log) toast_log = (ToastLog *)ps_calloc(TOAST_LOG, sizeof(ToastLog));
  if (!toast_log) return;
  char line[96];
  size_t n = strlcpy(line, text, sizeof(line));
  // 잘렸으면 한글(UTF-8 여러 바이트) 글자 중간에서 끊기지 않게 글자 앞까지 물린다
  if (n >= sizeof(line)) {
    size_t end = sizeof(line) - 1;
    while (end > 0 && ((uint8_t)text[end] & 0xC0) == 0x80) end--;  // text[end] 은 잘려 나간 첫 바이트
    line[end] = '\0';
  }
  for (char *p = line; *p; p++) {
    if (*p == '\n') *p = ' ';
  }
  // 같은 알림이 곧바로 또 뜨면 (10초 안) 시각만 고친다
  if (toast_log_count) {
    ToastLog &prev = toast_log[(toast_log_head + TOAST_LOG - 1) % TOAST_LOG];
    if (strcmp(prev.text, line) == 0 && millis() - prev.ms < 10000) {
      time_t now = time(nullptr);
      prev.at = now > 1700000000 ? now : 0;
      prev.ms = millis();
      return;
    }
  }
  time_t now = time(nullptr);
  ToastLog &e = toast_log[toast_log_head];
  e.at = now > 1700000000 ? now : 0;
  e.ms = millis();
  strlcpy(e.text, line, sizeof(e.text));
  toast_log_head = (toast_log_head + 1) % TOAST_LOG;
  if (toast_log_count < TOAST_LOG) toast_log_count++;
}

void ui_toast_history(char *out, size_t size) {
  out[0] = '\0';
  size_t used = 0;
  for (int i = 0; i < toast_log_count && used < size; i++) {
    const ToastLog &e = toast_log[(toast_log_head + TOAST_LOG - 1 - i) % TOAST_LOG];
    char when[16];
    if (e.at) {
      struct tm t;
      localtime_r(&e.at, &t);
      strftime(when, sizeof(when), "%H:%M", &t);
    } else {
      snprintf(when, sizeof(when), "켠 뒤 %lu분", (unsigned long)(e.ms / 60000));
    }
    used += snprintf(out + used, size - used, "%s%s  %s", i ? "\n" : "", when, e.text);
  }
  if (used >= size) out[size - 1] = '\0';
}

static void toast_hide(lv_timer_t *t) {
  lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
  lv_timer_pause(t);
}

void ui_toast(const char *text, uint32_t ms) {
  log_toast(text);
  if (!toast) {
    // 모든 화면(카메라 포함) 위에 뜨는 알림
    toast = lv_label_create(lv_layer_sys());
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
  // 화면 폭에 맞춘다: 큰 글자로 한 줄이 넘치면 작은 글자로, 그래도 넘치면 줄을 바꾼다
  int max_w = lv_disp_get_hor_res(NULL) - 16 - 2 * (12 + 2);
  const lv_font_t *font = &font_ko_20;
  lv_point_t size;
  lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  if (size.x > max_w) {
    font = &font_ko_14;
    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  }
  lv_obj_set_style_text_font(toast, font, 0);
  lv_label_set_long_mode(toast, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(toast, size.x > max_w ? max_w + 2 * (12 + 2) : LV_SIZE_CONTENT);
  lv_label_set_text(toast, text);
  lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(toast, LV_ALIGN_CENTER, 0, 0);
  lv_timer_set_period(toast_timer, ms);
  lv_timer_reset(toast_timer);
  lv_timer_resume(toast_timer);
  // 화면이 꺼져 있었으면 켜서 보이게 한다
  display_poke();
}
