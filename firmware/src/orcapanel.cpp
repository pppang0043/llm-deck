#include "orcapanel.h"

#include "askpanel.h"
#include "link.h"
#include "ui.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define COLOR_BG lv_color_hex(0x101418)
#define COLOR_DIM lv_color_hex(0x8A939E)
#define COLOR_ACCENT lv_color_hex(0xD97757)
#define COLOR_TALKING lv_color_hex(0xC8372D)
#define MAX_ROWS 24  // companion/orca.py 의 MAX_SESSIONS
#define COLS 2
#define ROWS 3
#define PER_PAGE (COLS * ROWS)
#define MAX_PAGES ((MAX_ROWS + PER_PAGE - 1) / PER_PAGE)
#define HEAD_H 20  // 버튼 위 프로젝트 이름 자리
#define GAP 6
#define KEEP_ORDER_MS 15000  // 누른 뒤 이만큼은 순서를 바꾸지 않는다 (누르려던 자리에 다른 세션이 오지 않게)

struct Session {
  char id[12];
  char title[96];
  char project[64];
  char state[16];
};
static Session sessions[MAX_ROWS];
static int count = 0;
static char active_id[12] = "";  // 이 기기에서 마지막으로 띄운 세션 (한 번 더 누르면 받아쓰기)
static bool orca_ok = false;     // PC 에서 ORCA 를 읽었는지
static bool received = false;    // PC 가 한 번이라도 보냈는지
static uint32_t touched_at = 0;

struct Page {
  lv_obj_t *page, *index;
  lv_obj_t *head[PER_PAGE];  // 버튼 위의 프로젝트 이름 (프로젝트가 바뀌는 버튼, 쪽의 첫 버튼에만)
  lv_obj_t *btn[PER_PAGE], *lbl[PER_PAGE];
  int session[PER_PAGE];  // 버튼마다 보여 주는 세션 번호 (-1 이면 빈 칸)
};
static Page pages[MAX_PAGES];
static int page_count = 0;
static lv_obj_t *panel, *pager, *state_lbl;
static int label_w = 80;  // 버튼 안 글자 폭 (orcapanel_layout)
static int btn_w = 100, btn_h = 60;

// 작업 상태는 버튼 색으로 (orca.py 의 s)
struct StateLook {
  const char *state;
  uint32_t color;
  bool dark_text;
};
static const StateLook LOOKS[] = {
    {"working", 0x2F6FD0, false},    {"waiting", 0xE0A030, true},   {"awaiting-input", 0xE0A030, true},
    {"permission", 0xD8433A, false}, {"blocked", 0xD8433A, false}, {"done", 0x2E8B5E, false},
    {"idle", 0x4A535E, false},       {"shell", 0x2A3038, false},
};

static const StateLook &look(const char *state) {
  for (const StateLook &l : LOOKS) {
    if (strcmp(l.state, state) == 0) return l;
  }
  return LOOKS[6];  // 모르는 상태는 "대기" 색
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

// 처음 누르면 그 세션을 ORCA 에서 앞으로 띄우기만 한다. 띄운 세션을 다시 누르면 받아쓰기, 또 누르면 넣는다.
static void btn_event(lv_event_t *e) {
  int i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
  if (i < 0 || i >= count) return;
  touched_at = lv_tick_get();
  const Session &s = sessions[i];
  bool busy = askpanel_dictating() || askpanel_dictate_step() == DICTATE_WRITING;
  if (!busy && strcmp(active_id, s.id) != 0) {
    if (!pc_connected()) {
      ui_toast("연결된 PC가 없습니다", 2000);
      return;
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"orca\":\"activate\",\"id\":\"%s\"}", s.id);
    link_send_event(json);
    snprintf(active_id, sizeof(active_id), "%s", s.id);
    orcapanel_refresh();
    return;
  }
  if (askpanel_dictating()) {
    // 같은 세션을 다시 누르면 끝내고 그 세션에 넣는다
    if (strcmp(askpanel_dictate_target(), s.id) == 0) {
      askpanel_release();
    } else {
      ui_toast("받아쓰기 중입니다\n빨간 버튼을 눌러 끝내 주세요", 2500);
    }
    return;
  }
  if (askpanel_dictate_step() == DICTATE_WRITING) {
    ui_toast("앞의 받아쓰기를 넣는 중입니다", 2000);
    return;
  }
  if (askpanel_press_orca(s.id)) orcapanel_refresh();
}

// 버튼 글자: 버튼에 들어가는 만큼(세 줄까지), 넘치면 ... 으로. 짧으면 버튼 가운데에.
static void set_btn_text(lv_obj_t *lbl, const char *text) {
  lv_point_t size;
  lv_txt_get_size(&size, text, &font_ko_14, 0, 0, label_w, LV_TEXT_FLAG_NONE);
  int line = lv_font_get_line_height(&font_ko_14);
  int max_h = LV_MIN(3, LV_MAX(1, (btn_h - 12) / line)) * line;
  bool cut = size.y > max_h;
  lv_label_set_long_mode(lbl, cut ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
  lv_obj_set_size(lbl, label_w, cut ? max_h : LV_SIZE_CONTENT);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
}

// 세션들을 6칸씩 쪽에 담는다 (PC 가 보낸 순서: 프로젝트별로 모아 최근에 연 것이 앞)
static void lay_out_pages() {
  page_count = (count + PER_PAGE - 1) / PER_PAGE;
  for (int n = 0; n < page_count; n++) {
    for (int k = 0; k < PER_PAGE; k++) {
      int i = n * PER_PAGE + k;
      pages[n].session[k] = i < count ? i : -1;
    }
  }
}

void orcapanel_refresh() {
  if (!panel) return;
  const char *problem = !pc_connected() ? "PC 연결 없음"
                        : !received    ? "PC에서 읽는 중..."
                        : !orca_ok     ? "PC에서 ORCA를 찾지 못함"
                        : count == 0   ? "ORCA에 열린 세션이 없습니다"
                                       : nullptr;
  if (problem) {
    lv_label_set_text(state_lbl, problem);
    lv_obj_clear_flag(state_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(state_lbl, LV_OBJ_FLAG_HIDDEN);
  }
  for (int n = 0; n < MAX_PAGES; n++) {
    Page &p = pages[n];
    if (n >= page_count || problem) {
      lv_obj_add_flag(p.page, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(p.page, LV_OBJ_FLAG_HIDDEN);
    if (page_count > 1) {
      lv_label_set_text_fmt(p.index, "%d/%d", n + 1, page_count);
    } else {
      lv_label_set_text(p.index, "");
    }
    for (int k = 0; k < PER_PAGE; k++) {
      int i = p.session[k];
      // 앞 버튼과 프로젝트가 이어지지 않으면 (또는 쪽의 첫 버튼이면) 위에 프로젝트 이름
      bool titled = i >= 0 && (k == 0 || strcmp(sessions[i].project, sessions[i - 1].project) != 0);
      if (titled) {
        lv_label_set_text(p.head[k], sessions[i].project);
        lv_obj_clear_flag(p.head[k], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(p.head[k], LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_set_user_data(p.btn[k], (void *)(intptr_t)i);
      if (i < 0) {
        lv_obj_add_flag(p.btn[k], LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_clear_flag(p.btn[k], LV_OBJ_FLAG_HIDDEN);
      const Session &s = sessions[i];
      const StateLook &l = look(s.state);
      bool mine = dictating_to(s);
      bool active = strcmp(active_id, s.id) == 0;
      lv_obj_set_style_bg_color(p.btn[k], mine ? COLOR_TALKING : lv_color_hex(l.color), 0);
      lv_obj_set_style_border_width(p.btn[k], active ? 3 : 0, 0);
      lv_color_t text = l.dark_text && !mine ? lv_color_hex(0x1A1A1A) : lv_color_white();
      lv_obj_set_style_text_color(p.lbl[k], text, 0);
      if (mine) {
        int step = askpanel_dictate_step();
        set_btn_text(p.lbl[k], !askpanel_dictating()          ? "넣는 중..."
                               : step == DICTATE_PREPARING ? "마이크 준비 중..."
                                                           : LV_SYMBOL_AUDIO " 말하세요\n다시 누르면 입력");
      } else {
        set_btn_text(p.lbl[k], s.title);
      }
    }
  }
}

void orcapanel_set(JsonObjectConst orca) {
  received = true;
  orca_ok = orca["ok"] | false;
  static Session next[MAX_ROWS];
  int n = 0;
  for (JsonObjectConst item : orca["list"].as<JsonArrayConst>()) {
    if (n >= MAX_ROWS) break;
    Session &s = next[n++];
    snprintf(s.id, sizeof(s.id), "%s", item["id"] | "");
    snprintf(s.title, sizeof(s.title), "%s", item["t"] | "");
    snprintf(s.project, sizeof(s.project), "%s", item["p"] | "");
    snprintf(s.state, sizeof(s.state), "%s", item["s"] | "idle");
  }
  // 방금 눌렀으면 같은 세션들인 동안은 자리를 그대로 두고 상태만 바꾼다
  bool keep = n == count && touched_at && lv_tick_elaps(touched_at) < KEEP_ORDER_MS;
  for (int i = 0; keep && i < n; i++) {
    int j = 0;
    while (j < n && strcmp(next[j].id, sessions[i].id) != 0) j++;
    if (j == n) {
      keep = false;
    } else {
      snprintf(sessions[i].state, sizeof(sessions[i].state), "%s", next[j].state);
      snprintf(sessions[i].title, sizeof(sessions[i].title), "%s", next[j].title);
    }
  }
  if (!keep) {
    memcpy(sessions, next, sizeof(Session) * n);
    count = n;
    lay_out_pages();
  }
  orcapanel_refresh();
}

void orcapanel_build(lv_obj_t *parent) {
  panel = lv_obj_create(parent);
  lv_obj_remove_style_all(panel);
  lv_obj_set_style_bg_color(panel, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  state_lbl = add_label(panel, "", &font_ko_20, COLOR_DIM);
  lv_obj_set_style_text_align(state_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(state_lbl, LV_LABEL_LONG_WRAP);

  // 쪽들을 가로로 늘어놓고 좌우로 밀어 한 쪽씩 본다. 첫 쪽에서 오른쪽으로 밀면 홈 (ui.cpp)
  pager = lv_obj_create(panel);
  lv_obj_remove_style_all(pager);
  lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
  lv_obj_set_scroll_dir(pager, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(pager, LV_SCROLL_SNAP_START);
  lv_obj_add_flag(pager, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
  for (Page &p : pages) {
    p.page = lv_obj_create(pager);
    lv_obj_remove_style_all(p.page);
    lv_obj_clear_flag(p.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(p.page, 8, 0);
    for (lv_obj_t *&head : p.head) {
      head = add_label(p.page, "", &font_ko_14, COLOR_ACCENT);
      lv_label_set_long_mode(head, LV_LABEL_LONG_DOT);
    }
    p.index = add_label(p.page, "", &font_ko_14, COLOR_DIM);
    lv_obj_align(p.index, LV_ALIGN_TOP_RIGHT, 0, 0);
    for (int k = 0; k < PER_PAGE; k++) {
      p.btn[k] = lv_btn_create(p.page);
      lv_obj_set_style_radius(p.btn[k], 10, 0);
      lv_obj_set_style_shadow_width(p.btn[k], 0, 0);
      lv_obj_set_style_pad_all(p.btn[k], 6, 0);
      lv_obj_set_style_border_color(p.btn[k], lv_color_white(), 0);
      lv_obj_add_event_cb(p.btn[k], btn_event, LV_EVENT_CLICKED, NULL);
      p.lbl[k] = add_label(p.btn[k], "", &font_ko_14, lv_color_white());
      lv_obj_set_style_text_align(p.lbl[k], LV_TEXT_ALIGN_CENTER, 0);
      lv_label_set_long_mode(p.lbl[k], LV_LABEL_LONG_DOT);
      lv_obj_add_flag(p.btn[k], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(p.page, LV_OBJ_FLAG_HIDDEN);
  }
  orcapanel_refresh();
}

void orcapanel_layout(int x, int y, int w, int h) {
  lv_obj_set_size(panel, w, h);
  lv_obj_set_pos(panel, x, y);
  lv_obj_set_width(state_lbl, w - 20);
  lv_obj_align(state_lbl, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_size(pager, w, h);
  lv_obj_set_pos(pager, 0, 0);
  // 버튼마다 위에 프로젝트 이름 자리를 둔다
  int inner_w = w - 16, inner_h = h - 16 - ROWS * HEAD_H;
  btn_w = (inner_w - GAP * (COLS - 1)) / COLS;
  btn_h = (inner_h - GAP * (ROWS - 1)) / ROWS;
  label_w = btn_w - 12;
  for (Page &p : pages) {
    lv_obj_set_size(p.page, w, h);
    for (int k = 0; k < PER_PAGE; k++) {
      int x = (k % COLS) * (btn_w + GAP), y = (k / COLS) * (HEAD_H + btn_h + GAP);
      lv_obj_set_pos(p.head[k], x + 2, y + 1);
      lv_obj_set_width(p.head[k], k == COLS - 1 ? btn_w - 34 : btn_w - 2);  // 첫 줄 오른쪽은 쪽 번호 자리
      lv_obj_set_size(p.btn[k], btn_w, btn_h);
      lv_obj_set_pos(p.btn[k], x, y + HEAD_H);
      lv_obj_set_width(p.lbl[k], label_w);
    }
  }
  orcapanel_refresh();  // 글자 폭이 바뀌었다
}

void orcapanel_show(bool on) {
  if (on) {
    orcapanel_refresh();
    lv_obj_scroll_to_x(pager, 0, LV_ANIM_OFF);  // 늘 가장 최근 쪽부터
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
  } else {
    // 받아쓰기는 다른 화면으로 가도 이어진다 (사용량 화면 버튼과 같다)
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  }
}
