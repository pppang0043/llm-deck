#include "orcapanel.h"

#include "askpanel.h"
#include "link.h"
#include "ui.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define COLOR_BG lv_color_hex(0x101418)
#define COLOR_DIM lv_color_hex(0x8A939E)
#define COLOR_ACCENT lv_color_hex(0xD97757)
#define COLOR_ROW lv_color_hex(0x1C232B)
#define COLOR_TALKING lv_color_hex(0xC8372D)
#define MAX_ROWS 8  // companion/orca.py 의 MAX_SESSIONS
#define ROW_H 50
#define BADGE_W 66

struct Session {
  char id[12];
  char title[96];
  char project[64];
  char state[16];
};
static Session sessions[MAX_ROWS];
static int count = 0;
static bool orca_ok = false;   // PC 에서 ORCA 를 읽었는지
static bool received = false;  // PC 가 한 번이라도 보냈는지

struct Row {
  lv_obj_t *btn, *badge, *badge_lbl, *title, *sub;
};
static Row rows[MAX_ROWS];
static lv_obj_t *panel, *state_lbl, *list, *empty_lbl;

// 상태 이름 -> 보이는 말과 색 (orca.py 의 s)
struct StateLook {
  const char *state, *text;
  uint32_t color;
};
static const StateLook LOOKS[] = {
    {"working", "작업 중", 0x3A7BD5},   {"waiting", "입력 대기", 0xE0A030}, {"awaiting-input", "입력 대기", 0xE0A030},
    {"permission", "권한 요청", 0xE0483C}, {"blocked", "막힘", 0xE0483C},     {"done", "완료", 0x2E7D5B},
    {"idle", "대기", 0x5A6470},         {"shell", "터미널", 0x3A424C},
};

static const StateLook &look(const char *state) {
  for (const StateLook &l : LOOKS) {
    if (strcmp(l.state, state) == 0) return l;
  }
  return LOOKS[6];  // 모르는 상태는 "대기"
}

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, color, 0);
  return lbl;
}

static bool pc_connected() {
  return link_active() != LINK_NONE && link_active() != LINK_CLOUD;
}

// 이 세션에 받아쓰기하는 중인지 (듣는 중, 받아 적는 중)
static bool dictating_to(const Session &s) {
  if (strcmp(askpanel_dictate_target(), s.id) != 0) return false;
  return askpanel_dictating() || askpanel_dictate_step() != DICTATE_IDLE;
}

static void row_event(lv_event_t *e) {
  int i = (intptr_t)lv_event_get_user_data(e);
  if (i >= count) return;
  const Session &s = sessions[i];
  if (askpanel_dictating()) {
    // 같은 세션을 다시 누르면 끝내고 그 세션에 넣는다
    if (strcmp(askpanel_dictate_target(), s.id) == 0) {
      askpanel_release();
    } else {
      ui_toast("받아쓰기 중입니다\n듣는 중인 버튼을 다시 눌러 끝내 주세요", 2500);
    }
    return;
  }
  if (askpanel_dictate_step() == DICTATE_WRITING) {
    ui_toast("앞의 받아쓰기를 넣는 중입니다", 2000);
    return;
  }
  if (askpanel_press_orca(s.id)) orcapanel_refresh();
}

void orcapanel_refresh() {
  if (!panel) return;
  if (!pc_connected()) {
    lv_label_set_text(state_lbl, "PC 연결 없음");
  } else if (!received) {
    lv_label_set_text(state_lbl, "PC에서 읽는 중...");
  } else if (!orca_ok) {
    lv_label_set_text(state_lbl, "PC에서 ORCA를 찾지 못함");
  } else {
    lv_label_set_text(state_lbl, "눌러 말하고 다시 누르면 입력");
  }
  for (int i = 0; i < MAX_ROWS; i++) {
    Row &r = rows[i];
    if (i >= count) {
      lv_obj_add_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const Session &s = sessions[i];
    const StateLook &l = look(s.state);
    lv_obj_clear_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(r.badge_lbl, l.text);
    lv_obj_set_style_bg_color(r.badge, lv_color_hex(l.color), 0);
    lv_label_set_text(r.title, s.title);
    bool mine = dictating_to(s);
    lv_obj_set_style_bg_color(r.btn, mine ? COLOR_TALKING : COLOR_ROW, 0);
    lv_obj_set_style_text_color(r.sub, mine ? lv_color_white() : COLOR_DIM, 0);
    if (mine) {
      int step = askpanel_dictate_step();
      lv_label_set_text(r.sub, !askpanel_dictating()          ? "받아 적어 넣는 중..."
                               : step == DICTATE_PREPARING ? "마이크 준비 중..."
                                                           : LV_SYMBOL_AUDIO " 말하세요... 다시 누르면 입력");
    } else {
      lv_label_set_text(r.sub, s.project);
    }
  }
  bool none = received && orca_ok && count == 0;
  if (none) {
    lv_obj_clear_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
  }
}

void orcapanel_set(JsonObjectConst orca) {
  received = true;
  orca_ok = orca["ok"] | false;
  count = 0;
  for (JsonObjectConst item : orca["list"].as<JsonArrayConst>()) {
    if (count >= MAX_ROWS) break;
    Session &s = sessions[count++];
    snprintf(s.id, sizeof(s.id), "%s", item["id"] | "");
    snprintf(s.title, sizeof(s.title), "%s", item["t"] | "");
    snprintf(s.project, sizeof(s.project), "%s", item["p"] | "");
    snprintf(s.state, sizeof(s.state), "%s", item["s"] | "idle");
  }
  orcapanel_refresh();
}

void orcapanel_build(lv_obj_t *parent) {
  panel = lv_obj_create(parent);
  lv_obj_remove_style_all(panel);
  lv_obj_set_style_bg_color(panel, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(panel, 10, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(add_label(panel, "ORCA", &font_ko_20, COLOR_ACCENT), LV_ALIGN_TOP_LEFT, 0, 0);
  state_lbl = add_label(panel, "", &font_ko_14, COLOR_DIM);
  lv_label_set_long_mode(state_lbl, LV_LABEL_LONG_DOT);

  // 세션 목록. 많으면 위아래로 밀어 본다.
  list = lv_obj_create(panel);
  lv_obj_remove_style_all(list);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 6, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  for (int i = 0; i < MAX_ROWS; i++) {
    Row &r = rows[i];
    r.btn = lv_btn_create(list);
    lv_obj_set_size(r.btn, lv_pct(100), ROW_H);
    lv_obj_set_style_radius(r.btn, 10, 0);
    lv_obj_set_style_shadow_width(r.btn, 0, 0);
    lv_obj_set_style_pad_hor(r.btn, 6, 0);
    lv_obj_set_style_pad_ver(r.btn, 5, 0);
    lv_obj_add_event_cb(r.btn, row_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    // 작업 상태 (색으로도 구분)
    r.badge = lv_obj_create(r.btn);
    lv_obj_remove_style_all(r.badge);
    lv_obj_set_size(r.badge, BADGE_W, 20);
    lv_obj_set_style_radius(r.badge, 6, 0);
    lv_obj_set_style_bg_opa(r.badge, LV_OPA_COVER, 0);
    lv_obj_clear_flag(r.badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(r.badge, LV_ALIGN_LEFT_MID, 0, 0);
    r.badge_lbl = add_label(r.badge, "", &font_ko_14, lv_color_white());
    lv_obj_center(r.badge_lbl);
    r.title = add_label(r.btn, "", &font_ko_14, lv_color_white());
    lv_label_set_long_mode(r.title, LV_LABEL_LONG_DOT);
    lv_obj_align(r.title, LV_ALIGN_TOP_LEFT, BADGE_W + 8, 0);
    // 프로젝트 (받아쓰기 중이면 진행 상황)
    r.sub = add_label(r.btn, "", &font_ko_14, COLOR_DIM);
    lv_label_set_long_mode(r.sub, LV_LABEL_LONG_DOT);
    lv_obj_align(r.sub, LV_ALIGN_BOTTOM_LEFT, BADGE_W + 8, 0);
    lv_obj_add_flag(r.btn, LV_OBJ_FLAG_HIDDEN);
  }
  empty_lbl = add_label(list, "ORCA에 열린 세션이 없습니다", &font_ko_14, COLOR_DIM);
  lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
  orcapanel_refresh();
}

void orcapanel_layout(int x, int y, int w, int h) {
  lv_obj_set_size(panel, w, h);
  lv_obj_set_pos(panel, x, y);
  int inner_w = w - 20, inner_h = h - 20;
  lv_obj_align(state_lbl, LV_ALIGN_TOP_LEFT, 64, 4);
  lv_obj_set_width(state_lbl, inner_w - 64);
  lv_obj_set_size(list, inner_w, inner_h - 34);
  lv_obj_set_pos(list, 0, 34);
  // 버튼 안 글자 폭 (버튼 안쪽 여백 6씩)
  int row_w = inner_w - 12;
  for (Row &r : rows) {
    lv_obj_set_width(r.title, row_w - BADGE_W - 8);
    lv_obj_set_width(r.sub, row_w - BADGE_W - 8);
  }
}

void orcapanel_show(bool on) {
  if (on) {
    orcapanel_refresh();
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
  } else {
    // 받아쓰기는 다른 화면으로 가도 이어진다 (사용량 화면 버튼과 같다)
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  }
}
