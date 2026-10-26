#include "pcmic.h"

#include "link.h"
#include "ui.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define COLOR_BG lv_color_hex(0x101418)
#define COLOR_DIM lv_color_hex(0x8A939E)
#define COLOR_ACCENT lv_color_hex(0xD97757)
#define COLOR_ROW lv_color_hex(0x1C232B)
#define COLOR_OK lv_color_hex(0x4CAF50)
#define COLOR_CHECKING lv_color_hex(0xE0A030)
#define COLOR_NONE lv_color_hex(0xE5533D)
#define MIC_MAX 8
#define ICON_W 12
#define ICON_H 20

enum { MIC_UNKNOWN, MIC_CHECKING, MIC_OK, MIC_NONE };
static int state = MIC_UNKNOWN;
static char name[128] = "", why[384] = "", pick[128] = "";
static char list[MIC_MAX][128];
static bool limited[MIC_MAX];  // 고르면 30분만 쓰는 마이크 (Mac 의 아이폰 마이크)
static int count = 0, left = 0;  // left: 고른 아이폰 마이크를 쓸 수 있는 남은 분

static lv_obj_t *icon, *capsule, *stem, *base, *cross;
static lv_obj_t *panel, *panel_state, *panel_list;

static bool pc_connected() {
  return link_active() != LINK_NONE && link_active() != LINK_CLOUD;
}

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, color, 0);
  return lbl;
}

static lv_obj_t *add_part(lv_obj_t *parent, int x, int y, int w, int h, int radius) {
  lv_obj_t *part = lv_obj_create(parent);
  lv_obj_remove_style_all(part);
  lv_obj_set_pos(part, x, y);
  lv_obj_set_size(part, w, h);
  lv_obj_set_style_radius(part, radius, 0);
  lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
  lv_obj_clear_flag(part, LV_OBJ_FLAG_CLICKABLE);
  return part;
}

static void close_panel(lv_event_t *e = nullptr) {
  if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

static void render_panel();

static void row_clicked(lv_event_t *e) {
  char chosen[128];  // 행의 값은 pick 자체일 수 있다 (연결 안 된 고른 마이크)
  snprintf(chosen, sizeof(chosen), "%s", (const char *)lv_event_get_user_data(e));
  JsonDocument doc;
  doc["mic_pick"] = chosen;
  char json[200];
  serializeJson(doc, json, sizeof(json));
  link_send_event(json);
  // PC 가 들어 보고 알려 줄 때까지 고른 것으로 보이고 "들어 보는 중"
  snprintf(pick, sizeof(pick), "%s", chosen);
  state = MIC_CHECKING;
  pcmic_refresh();
  close_panel();
  bool timed = false;
  for (int i = 0; i < count; i++) timed = timed || (limited[i] && strcmp(list[i], chosen) == 0);
  char text[180];
  snprintf(text, sizeof(text), timed ? "PC 마이크: %s\n30분 동안 씁니다" : "PC 마이크: %s\n들어 보는 중...",
           chosen[0] ? chosen : "자동");
  ui_toast(text, 2500);
}

static void add_row(const char *label, const char *value, bool selected) {
  lv_obj_t *row = lv_btn_create(panel_list);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(row, COLOR_ROW, 0);
  lv_obj_set_style_shadow_width(row, 0, 0);
  lv_obj_set_style_radius(row, 8, 0);
  lv_obj_set_style_pad_all(row, 10, 0);
  lv_obj_set_style_pad_right(row, selected ? 30 : 10, 0);  // 고른 줄은 오른쪽에 ✓
  lv_obj_set_style_border_color(row, COLOR_ACCENT, 0);
  lv_obj_set_style_border_width(row, selected ? 2 : 0, 0);
  // 버튼이 다시 그려져도 값이 남도록 행이 지워질 때까지 들고 있는다 (render_panel 이 행을 모두 지운다)
  lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)value);
  lv_obj_t *lbl = add_label(row, label, &font_ko_14, lv_color_white());
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);  // 긴 이름은 한 줄로 줄인다
  lv_obj_set_size(lbl, LV_PCT(100), lv_font_get_line_height(&font_ko_14));
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
  if (selected) lv_obj_align(add_label(row, LV_SYMBOL_OK, &lv_font_montserrat_14, COLOR_ACCENT), LV_ALIGN_RIGHT_MID, 22, 0);
}

static void render_panel() {
  if (!panel || lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN)) return;
  char text[460];
  if (state == MIC_OK) {
    snprintf(text, sizeof(text), "쓰는 중: %s", name);
  } else if (state == MIC_NONE) {
    snprintf(text, sizeof(text), "연결 불가\n%s", why[0] ? why : "소리가 오는 마이크가 없습니다");
  } else {
    snprintf(text, sizeof(text), "마이크를 들어 보는 중...");
  }
  lv_label_set_text(panel_state, text);
  lv_obj_set_style_text_color(panel_state, state == MIC_OK ? COLOR_OK : state == MIC_NONE ? COLOR_NONE : COLOR_CHECKING, 0);

  lv_obj_clean(panel_list);
  static const char AUTO[] = "";
  add_row("자동 (추천)", AUTO, pick[0] == '\0');
  bool found = false;
  for (int i = 0; i < count; i++) {
    bool selected = pick[0] && strcmp(pick, list[i]) == 0;
    found = found || selected;
    if (limited[i]) {
      char label[160];
      if (selected && left > 0) {
        snprintf(label, sizeof(label), "%s (%d분 남음)", list[i], left);
      } else {
        snprintf(label, sizeof(label), "%s (30분만)", list[i]);
      }
      add_row(label, list[i], selected);
    } else {
      add_row(list[i], list[i], selected);
    }
  }
  if (pick[0] && !found) {
    // 고른 마이크가 빠져 있다 (PC 는 자동으로 고른다)
    static char missing[160];
    snprintf(missing, sizeof(missing), "%s (연결 안 됨)", pick);
    add_row(missing, pick, true);
  }
}

static void open_panel(lv_event_t *e) {
  if (!panel) {
    panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(panel, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);  // 아래 화면이 눌리지 않게
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 8, 0);

    lv_obj_t *head = lv_obj_create(panel);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), 32);
    lv_obj_align(add_label(head, "PC 마이크", &font_ko_20, COLOR_ACCENT), LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *close = lv_btn_create(head);
    lv_obj_set_size(close, 40, 28);
    lv_obj_set_style_bg_color(close, COLOR_ROW, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_set_ext_click_area(close, 8);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_center(add_label(close, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, lv_color_white()));
    lv_obj_add_event_cb(close, close_panel, LV_EVENT_CLICKED, NULL);

    panel_state = add_label(panel, "", &font_ko_14, COLOR_DIM);
    lv_obj_set_width(panel_state, LV_PCT(100));
    lv_label_set_long_mode(panel_state, LV_LABEL_LONG_WRAP);

    panel_list = lv_obj_create(panel);
    lv_obj_remove_style_all(panel_list);
    lv_obj_set_width(panel_list, LV_PCT(100));
    lv_obj_set_flex_grow(panel_list, 1);
    lv_obj_set_flex_flow(panel_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel_list, 6, 0);
    lv_obj_add_flag(panel_list, LV_OBJ_FLAG_SCROLLABLE);
  }
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
  render_panel();
}

void pcmic_build(lv_obj_t *scr) {
  icon = lv_obj_create(scr);
  lv_obj_remove_style_all(icon);
  lv_obj_set_size(icon, ICON_W, ICON_H);
  lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_set_ext_click_area(icon, 10);
  lv_obj_add_event_cb(icon, open_panel, LV_EVENT_CLICKED, NULL);
  // 마이크 모양: 둥근 머리, 기둥, 받침
  capsule = add_part(icon, 2, 0, 8, 13, 4);
  stem = add_part(icon, 5, 13, 2, 4, 0);
  base = add_part(icon, 1, 17, 10, 2, 1);
  // 연결 불가: 오른쪽 아래에 작은 ×
  cross = add_label(icon, LV_SYMBOL_CLOSE, &lv_font_montserrat_10, COLOR_NONE);
  lv_obj_set_pos(cross, 9, 8);
  lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
}

int pcmic_place(lv_obj_t *left) {
  lv_obj_update_layout(lv_obj_get_parent(left));
  int right = lv_obj_get_x(left) + lv_obj_get_width(left);
  if (!icon || lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN)) return right;
  lv_obj_align_to(icon, left, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
  return right + 8 + ICON_W;
}

void pcmic_refresh() {
  if (!icon) return;
  if (!pc_connected()) {
    state = MIC_UNKNOWN;  // 다른 PC 가 붙을 수 있다
    close_panel();
  }
  if (state == MIC_UNKNOWN) {
    lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_color_t color = state == MIC_OK ? COLOR_OK : state == MIC_NONE ? COLOR_NONE : COLOR_CHECKING;
  for (lv_obj_t *part : {capsule, stem, base}) lv_obj_set_style_bg_color(part, color, 0);
  if (state == MIC_NONE) {
    lv_obj_clear_flag(cross, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(cross, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN);
}

void pcmic_set(JsonObjectConst mic) {
  const char *s = mic["state"] | "";
  int next = strcmp(s, "ok") == 0 ? MIC_OK : strcmp(s, "none") == 0 ? MIC_NONE : MIC_CHECKING;
  bool lost = next == MIC_NONE && (state == MIC_OK || state == MIC_CHECKING);
  bool was_hidden = !icon || lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN);
  state = next;
  snprintf(name, sizeof(name), "%s", mic["name"] | "");
  snprintf(why, sizeof(why), "%s", mic["why"] | "");
  snprintf(pick, sizeof(pick), "%s", mic["pick"] | "");
  count = 0;
  for (JsonVariantConst item : mic["list"].as<JsonArrayConst>()) {
    if (count >= MIC_MAX) break;
    limited[count] = false;
    for (JsonVariantConst l : mic["limited"].as<JsonArrayConst>()) limited[count] = limited[count] || strcmp(l | "", item | "") == 0;
    snprintf(list[count++], sizeof(list[0]), "%s", item | "");
  }
  left = mic["left"] | 0;
  pcmic_refresh();
  // 상태줄 오른쪽(시각)이 표시와 겹치지 않게 다시 놓는다
  if (was_hidden) ui_layout_status();
  render_panel();
  if (lost) ui_toast("PC 마이크를 쓸 수 없습니다\n위 마이크 표시를 눌러 확인하세요", 3000);
}

const char *pcmic_why() {
  return state == MIC_NONE ? why : "";
}
