#include "orcapanel.h"

#include "askpanel.h"
#include "link.h"
#include "pcpanel.h"
#include "speaker.h"
#include "ui.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define COLOR_BG lv_color_hex(0x101418)
#define COLOR_DIM lv_color_hex(0x8A939E)
#define COLOR_ACCENT lv_color_hex(0xD97757)
#define COLOR_TALKING lv_color_hex(0xC8372D)
#define MAX_ROWS 24  // companion/orca.py 의 MAX_SESSIONS
#define PER_PAGE 6  // 가로 화면 2줄 x 3칸, 세로 화면 3줄 x 2칸 (orcapanel_layout)
#define MAX_PAGES ((MAX_ROWS + PER_PAGE - 1) / PER_PAGE)
#define HEAD_H 20  // 버튼 위 프로젝트 이름 자리
#define GAP 6
// 세션을 띄우면 아래에서 세션 버튼 위에 겹쳐 뜨는 단축키 팝업 (버튼을 여러 칸으로)
#define POP_BTN_H 32
#define POP_MAX 13  // 키워드 7개 (pcpanel 의 KW_MAX) + 지우기 + 맨 아래 줄 5개
#define POP_ROWS 3  // 키워드 두 줄 (둘째 줄 끝에 지우기), 맨 아래 줄: 녹음, Tab, Tab + Enter, Enter, Esc
#define POP_BTN_MIN_W 52  // 버튼이 이보다 좁아지지 않는다. 줄에 다 안 들어가면 그 줄을 옆으로 밀어 본다
#define POP_FIT_MAX 4     // 버튼이 이만큼 이하인 줄은 밀지 않고 한 화면에 다 넣는다 (넘치면 버튼을 줄인다)
#define POP_DICTATE_COLOR 0x7A2E2A
#define POP_DRAG_START 10  // 이만큼 내려야 끌기 (그 전에는 버튼 누르기)
#define POP_DRAG_CLOSE 40  // 놓을 때 이만큼 내렸으면 닫는다
#define KEEP_ORDER_MS 15000  // 누른 뒤 이만큼은 순서를 바꾸지 않는다 (누르려던 자리에 다른 세션이 오지 않게)
#define ALERT_QUIET_MS 60000  // 한 세션의 같은 알림은 이만큼 안에 다시 내지 않는다 (상태가 오락가락할 때)

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
static int cols = 3, rows = 2;
// 오른쪽 팝업: 띄운 세션(active_id)에 보낼 키워드와 키. 닫으면 다음에 세션을 띄울 때 다시 뜬다.
static lv_obj_t *pop, *pop_title, *pop_list;
static lv_obj_t *pop_rows[POP_ROWS];  // 줄마다 따로 옆으로 밀린다
static lv_obj_t *pop_dictate;  // 녹음 버튼 (받아쓰는 중이면 빨갛게, 글자가 바뀐다)
static int pop_count = 0;
static bool pop_open = false;
static int area_w, area_h;  // 마지막 배치 (팝업 자리를 정한다)
static void set_pop(bool open);
// 빠른 답 창: 세션을 길게 누르면 가운데에 뜬다. 권한 요청은 허용 / 거부, 입력 기다림·완료는 계속.
static lv_obj_t *qr, *qr_title, *qr_btn[2], *qr_lbl[2];
static char qr_id[12] = "";  // 빠른 답을 보낼 세션 (비어 있으면 닫혀 있다)
static bool qr_permission = false;
static bool long_pressed = false;  // 길게 누른 뒤 손을 떼면 오는 CLICKED 는 무시한다
static void close_qr();

// 작업 상태는 버튼 색으로 (orca.py 의 s)
struct StateLook {
  const char *state;
  uint32_t color;
  bool dark_text;
};
static const StateLook LOOKS[] = {
    {"working", 0x2B4A73, false},    {"waiting", 0xE0A030, true},   {"awaiting-input", 0xE0A030, true},
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

static bool is_permission(const char *state) {
  return strcmp(state, "permission") == 0;
}

// 다음 입력을 기다리는 상태 (orca.py 의 REPLIES "continue" 와 같다)
static bool is_waiting(const char *state) {
  return strcmp(state, "waiting") == 0 || strcmp(state, "awaiting-input") == 0 || strcmp(state, "done") == 0;
}

// 받아쓰기: 그 세션에 받아 적기 시작, 받아쓰는 중이면 끝내고 넣는다 (세션 버튼을 다시 누르기, 팝업의 받아쓰기 버튼)
static void dictate(const char *id) {
  if (askpanel_dictating()) {
    if (strcmp(askpanel_dictate_target(), id) == 0) {
      askpanel_release();
    } else {
      ui_toast("받아쓰기 중입니다\n빨간 버튼을 눌러 끝내 주세요", 2500);
    }
    return;
  }
  // 앞의 받아쓰기를 넣는 중이어도 된다 (PC 가 줄을 세워 차례대로 넣는다)
  if (askpanel_press_orca(id)) orcapanel_refresh();
}

// 처음 누르면 그 세션을 ORCA 에서 앞으로 띄우기만 한다. 띄운 세션을 다시 누르면 받아쓰기, 또 누르면 넣는다.
static void btn_event(lv_event_t *e) {
  int i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
  if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
    long_pressed = false;
    return;
  }
  if (long_pressed) return;
  if (i < 0 || i >= count) return;
  touched_at = lv_tick_get();
  const Session &s = sessions[i];
  if (!askpanel_dictating() && strcmp(active_id, s.id) != 0) {
    if (!pc_connected()) {
      ui_toast("연결된 PC가 없습니다", 2000);
      return;
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"orca\":\"activate\",\"id\":\"%s\"}", s.id);
    link_send_event(json);
    snprintf(active_id, sizeof(active_id), "%s", s.id);
    set_pop(true);
    orcapanel_refresh();
    return;
  }
  dictate(s.id);
}

// 길게 누르면 빠른 답 창 (누르기는 그대로: 띄우기, 받아쓰기)
static void btn_long_event(lv_event_t *e) {
  int i = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
  if (i < 0 || i >= count) return;
  long_pressed = true;
  touched_at = lv_tick_get();
  const Session &s = sessions[i];
  if (!pc_connected()) {
    ui_toast("연결된 PC가 없습니다", 2000);
    return;
  }
  if (!is_permission(s.state) && !is_waiting(s.state)) {
    ui_toast("빠른 답은 권한 요청이나\n입력 기다림일 때만 됩니다", 2500);
    return;
  }
  snprintf(qr_id, sizeof(qr_id), "%s", s.id);
  qr_permission = is_permission(s.state);
  lv_label_set_text(qr_title, s.title);
  lv_label_set_text(qr_lbl[0], qr_permission ? "허용" : "계속");
  lv_obj_set_style_bg_color(qr_btn[0], lv_color_hex(0x2E8B5E), 0);
  if (qr_permission) {
    lv_label_set_text(qr_lbl[1], "거부");
    lv_obj_clear_flag(qr_btn[1], LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(qr_btn[1], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_clear_flag(qr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(qr);
}

// 빠른 답 버튼: 이름만 보낸다. 보낼 키와 지금 상태 확인은 PC 가 한다 (orca.py 의 REPLIES)
static void qr_event(lv_event_t *e) {
  int k = (intptr_t)lv_event_get_user_data(e);
  if (!qr_id[0]) return;
  if (!pc_connected()) {
    ui_toast("연결된 PC가 없습니다", 2000);
    return;
  }
  touched_at = lv_tick_get();
  const char *reply = !qr_permission ? "continue" : k == 0 ? "allow" : "deny";
  char json[96];
  snprintf(json, sizeof(json), "{\"orca\":\"reply\",\"id\":\"%s\",\"reply\":\"%s\"}", qr_id, reply);
  link_send_event(json);
  close_qr();
}

static void close_qr() {
  qr_id[0] = 0;
  if (qr) lv_obj_add_flag(qr, LV_OBJ_FLAG_HIDDEN);
}

static void build_qr() {
  qr = lv_obj_create(panel);
  lv_obj_remove_style_all(qr);
  lv_obj_set_style_bg_color(qr, lv_color_hex(0x1C232B), 0);
  lv_obj_set_style_bg_opa(qr, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(qr, 12, 0);
  lv_obj_set_style_pad_all(qr, 8, 0);
  lv_obj_set_style_border_width(qr, 1, 0);
  lv_obj_set_style_border_color(qr, lv_color_hex(0x3A4350), 0);
  lv_obj_set_style_shadow_width(qr, 24, 0);
  lv_obj_set_style_shadow_color(qr, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(qr, LV_OPA_80, 0);
  lv_obj_clear_flag(qr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(qr, LV_OBJ_FLAG_HIDDEN);
  qr_title = add_label(qr, "", &font_ko_14, COLOR_ACCENT);
  lv_label_set_long_mode(qr_title, LV_LABEL_LONG_DOT);
  lv_obj_align(qr_title, LV_ALIGN_TOP_LEFT, 0, 2);
  lv_obj_t *close = lv_btn_create(qr);
  lv_obj_set_size(close, 24, 22);
  lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(close, 0, 0);
  lv_obj_set_style_pad_all(close, 0, 0);
  lv_obj_set_ext_click_area(close, 8);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_add_event_cb(close, [](lv_event_t *e) { close_qr(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_center(add_label(close, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, COLOR_DIM));
  for (int k = 0; k < 2; k++) {
    qr_btn[k] = lv_btn_create(qr);
    lv_obj_set_style_radius(qr_btn[k], 8, 0);
    lv_obj_set_style_shadow_width(qr_btn[k], 0, 0);
    lv_obj_set_style_bg_color(qr_btn[k], lv_color_hex(k == 0 ? 0x2E8B5E : 0xD8433A), 0);
    lv_obj_add_event_cb(qr_btn[k], qr_event, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    qr_lbl[k] = add_label(qr_btn[k], "", &font_ko_20, lv_color_white());
    lv_obj_center(qr_lbl[k]);
  }
}

// 가운데에 뜬다. 버튼 두 개(거부가 숨으면 계속 하나가 넓게)
static void place_qr() {
  int w = LV_MIN(area_w - 24, 320), h = 112;
  lv_obj_set_size(qr, w, h);
  lv_obj_align(qr, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_width(qr_title, w - 16 - 28);
  int bw = (w - 16 - 8) / 2, bh = h - 16 - 32;
  lv_obj_set_size(qr_btn[0], bw, bh);
  lv_obj_set_pos(qr_btn[0], 0, 32);
  lv_obj_set_size(qr_btn[1], bw, bh);
  lv_obj_set_pos(qr_btn[1], bw + 8, 32);
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

// 세션들을 쪽(6칸)에 PC 가 보낸 순서(ORCA 화면의 프로젝트·탭 순서)대로 놓는다.
// 같은 프로젝트는 한 줄에 같이 둔다: 한 줄에 들어가는 프로젝트는 그만큼 빈 줄(앞쪽 줄의 남은 칸 포함)에 통째로,
// 한 줄보다 많으면 새 줄부터 이어서 (한 쪽에 들어가면 쪽을 넘기지 않는다).
static void lay_out_pages() {
  for (Page &p : pages) {
    for (int &i : p.session) i = -1;
  }
  page_count = 0;
  const int total_rows = MAX_PAGES * rows;
  int fill[MAX_PAGES * PER_PAGE] = {};  // 줄마다 채운 칸 수
  bool placed[MAX_ROWS] = {};
  int next_row = 0;  // 아직 아무것도 놓지 않은 첫 줄
  auto put = [&](int row, int i) {
    int n = row / rows;
    pages[n].session[(row % rows) * cols + fill[row]++] = i;
    placed[i] = true;
    if (n >= page_count) page_count = n + 1;
  };
  for (int i = 0; i < count;) {
    int n = 1;
    while (i + n < count && strcmp(sessions[i + n].project, sessions[i].project) == 0) n++;
    if (n <= cols) {
      int r = 0;
      while (r < next_row && fill[r] + n > cols) r++;
      if (r == next_row && next_row < total_rows) next_row++;
      if (r < next_row) {
        for (int k = 0; k < n; k++) put(r, i + k);
      }
    } else {
      int need = (n + cols - 1) / cols;
      if (need <= rows && next_row % rows + need > rows) next_row += rows - next_row % rows;
      for (int k = 0; k < n && (k % cols || next_row < total_rows); k++) {
        if (k % cols == 0) next_row++;
        put(next_row - 1, i + k);
      }
    }
    i += n;
  }
  // 칸이 모자라 못 놓은 세션은 남은 빈칸에 차례로
  for (int i = 0, r = 0; i < count; i++) {
    if (placed[i]) continue;
    while (r < total_rows && fill[r] == cols) r++;
    if (r == total_rows) break;
    put(r, i);
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
  // 팝업: 띄운 세션이 목록에서 사라졌으면 닫는다
  int active = 0;
  while (active < count && strcmp(sessions[active].id, active_id) != 0) active++;
  if (pop_open && (problem || active == count)) set_pop(false);
  if (pop_open) lv_label_set_text(pop_title, sessions[active].title);
  if (pop_open && pop_dictate) {
    bool mine = strcmp(askpanel_dictate_target(), active_id) == 0;
    int step = askpanel_dictate_step();
    lv_label_set_text(lv_obj_get_child(pop_dictate, 0), !mine || step == DICTATE_IDLE ? "녹음"
                                                        : !askpanel_dictating()     ? "넣는 중..."
                                                        : step == DICTATE_PREPARING ? "준비 중..."
                                                                                    : "입력");
    lv_obj_set_style_bg_color(pop_dictate, askpanel_dictating() && mine ? COLOR_TALKING : lv_color_hex(POP_DICTATE_COLOR), 0);
  }
  // 빠른 답 창: 세션이 사라졌거나 더는 그 답을 받을 상태가 아니면 닫는다
  if (qr_id[0]) {
    int q = 0;
    while (q < count && strcmp(sessions[q].id, qr_id) != 0) q++;
    bool fits = q < count && (qr_permission ? is_permission(sessions[q].state) : is_waiting(sessions[q].state));
    if (problem || !fits) close_qr();
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
    int prev = -1;  // 이 쪽에서 앞에 놓인 세션
    for (int k = 0; k < PER_PAGE; k++) {
      int i = p.session[k];
      // 프로젝트가 바뀌는 버튼과 쪽의 첫 버튼에만 위에 프로젝트 이름
      bool titled = i >= 0 && (prev < 0 || strcmp(sessions[i].project, sessions[prev].project) != 0);
      if (i >= 0) prev = i;
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

// 닫으면 띄운 세션도 잊는다: 다시 누르면 받아쓰기가 아니라 다시 띄우고 팝업을 연다
static void close_pop() {
  active_id[0] = 0;
  set_pop(false);
  orcapanel_refresh();
}

// 아래로 끌어 닫기: 팝업이 손가락을 따라 내려가고, 놓을 때 POP_DRAG_CLOSE 보다 내렸으면 마저 내려가며 닫힌다.
// 빠르기가 아니라 끈 거리로 정해서 미러링 창에서 마우스로 천천히 끌어도 된다 (LVGL 밀기 제스처는 빨라야 잡힌다).
static int drag_y0 = 0, drag_dy = 0;
static bool dragged = false;  // 이번 누르기가 끌기였다: 손을 뗄 때 오는 버튼 CLICKED 는 무시한다

static void pop_translate(void *obj, int32_t v) {
  lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}

static void pop_slide(int to, lv_anim_ready_cb_t done) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, pop);
  lv_anim_set_exec_cb(&a, pop_translate);
  lv_anim_set_values(&a, lv_obj_get_style_translate_y(pop, 0), to);
  lv_anim_set_time(&a, 150);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  if (done) lv_anim_set_ready_cb(&a, done);
  lv_anim_start(&a);
}

static void pop_drag(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  if (code == LV_EVENT_PRESSED) {
    lv_anim_del(pop, pop_translate);
    drag_y0 = p.y, drag_dy = 0;
    dragged = false;
    return;
  }
  int dy = LV_MAX(p.y - drag_y0, 0);
  if (code == LV_EVENT_PRESSING) {
    if (!dragged && dy < POP_DRAG_START) return;  // 누르다 조금 흔들린 것은 그냥 누르기
    if (!dragged && lv_indev_get_scroll_obj(indev)) return;  // 줄을 옆으로 미는 중 (조금 비스듬해도 닫기가 아니다)
    if (!dragged) lv_obj_clear_state(lv_event_get_target(e), LV_STATE_PRESSED);  // 끌기가 되면 버튼 눌림 표시를 뗀다
    dragged = true;
    drag_dy = dy;
    lv_obj_set_style_translate_y(pop, dy, 0);
    return;
  }
  // 놓음 (RELEASED / PRESS_LOST)
  if (!dragged) return;
  if (drag_dy >= POP_DRAG_CLOSE) {
    pop_slide(lv_obj_get_height(pop) + 8, [](lv_anim_t *a) { close_pop(); });
  } else {
    pop_slide(0, nullptr);
  }
}

static void add_pop_drag(lv_obj_t *obj) {
  lv_obj_add_event_cb(obj, pop_drag, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(obj, pop_drag, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(obj, pop_drag, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(obj, pop_drag, LV_EVENT_PRESS_LOST, NULL);
}

// 팝업 버튼: 띄운 세션에만 보낸다 (PC 의 앞 창이 아니라 ORCA 로, companion/orca.py)
static void pop_event(lv_event_t *e) {
  const char *key = (const char *)lv_event_get_user_data(e);
  if (dragged) return;  // 끌어서 닫는 중이었다
  if (!pc_connected()) {
    ui_toast("연결된 PC가 없습니다", 2000);
    return;
  }
  touched_at = lv_tick_get();
  if (strcmp(key, "dictate") == 0) {
    dictate(active_id);  // 팝업은 그대로: 받아 적은 글이 들어가면 이어서 Enter 를 누른다
    return;
  }
  char json[96];
  snprintf(json, sizeof(json), "{\"orca\":\"send\",\"id\":\"%s\",\"key\":\"%s\"}", active_id, key);
  link_send_event(json);
  // Tab, 지우기, Esc 만 빼면 모두 끝에 Enter 를 친다 (companion/orca.py 의 KEYS, 키워드): 보냈으니 닫는다.
  // /orchestration 은 Enter 없이 띄어쓰기만 넣으니 (companion/typer.py) 뒤에 할 일을 이어 넣도록 그대로 둔다
  if (strcmp(key, "tab") != 0 && strcmp(key, "erase") != 0 && strcmp(key, "esc") != 0 &&
      strcmp(key, "orchestration") != 0)
    close_pop();
}

static lv_obj_t *add_pop_btn(int row, const char *key, const char *name, uint32_t color) {
  if (pop_count >= POP_MAX) return nullptr;
  lv_obj_t *b = lv_btn_create(pop_rows[row]);
  pop_count++;
  lv_obj_set_user_data(b, (void *)name);  // 원래 이름 (라벨 글자는 넘치면 LVGL 이 "..." 로 바꿔 둔다)
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 2, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
  lv_obj_add_event_cb(b, pop_event, LV_EVENT_CLICKED, (void *)key);
  add_pop_drag(b);
  lv_obj_t *lbl = add_label(b, name, &font_ko_14, lv_color_white());
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);  // 한 줄로, 넘치면 ...
  lv_obj_set_size(lbl, LV_PCT(100), lv_font_get_line_height(&font_ko_14));
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(lbl);
  return b;
}

// 키워드 버튼 (PC 가 보낸 목록이면 그 이름) 두 줄 (윗줄이 하나 더 많게), 그 아래 키
static void fill_pop() {
  const char *id, *name;
  uint32_t color;
  int kw = 0;
  for (int i = 0; pcpanel_keyword(i, &id, &name, &color); i++) kw += strcmp(id, "tab_enter") != 0;
  int n = 0;
  for (int i = 0; pcpanel_keyword(i, &id, &name, &color); i++) {
    if (strcmp(id, "tab_enter") == 0) continue;  // 아래 키 버튼과 겹친다
    add_pop_btn(n++ < (kw + 1) / 2 ? 0 : 1, id, name, color);
  }
  // 입력칸에 쓴 글을 지운다 (Ctrl+U). / 명령이 아니지만 옆의 /clear 들과 같은 모양으로 (pcpanel 의 KW_SLASH_COLOR)
  add_pop_btn(1, "erase", "/지우기", 0x16808C);
  pop_dictate = add_pop_btn(2, "dictate", "녹음", POP_DICTATE_COLOR);
  add_pop_btn(2, "tab", "Tab", 0x3A4350);
  add_pop_btn(2, "tab_enter", "Tab + Enter", 0x5B6573);
  add_pop_btn(2, "enter", "Enter", 0x3A4350);
  add_pop_btn(2, "esc", "Esc", 0x3A4350);
}

static void place_pop();

static void build_pop() {
  pop = lv_obj_create(panel);
  lv_obj_remove_style_all(pop);
  lv_obj_set_style_bg_color(pop, lv_color_hex(0x1C232B), 0);
  lv_obj_set_style_bg_opa(pop, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pop, 12, 0);
  lv_obj_set_style_pad_all(pop, 6, 0);
  // 세션 버튼 위에 떠 있는 느낌: 테두리와 그림자
  lv_obj_set_style_border_width(pop, 1, 0);
  lv_obj_set_style_border_color(pop, lv_color_hex(0x3A4350), 0);
  lv_obj_set_style_shadow_width(pop, 24, 0);
  lv_obj_set_style_shadow_color(pop, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(pop, LV_OPA_80, 0);
  lv_obj_clear_flag(pop, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(pop, LV_OBJ_FLAG_SCROLL_CHAIN);  // 팝업 위에서 끌면 뒤의 화면이 아니라 아래 밀기가 된다
  lv_obj_add_flag(pop, LV_OBJ_FLAG_HIDDEN);
  add_pop_drag(pop);  // 팝업 어디서든(버튼 위 포함) 아래로 끌면 닫는다
  // 맨 위: 보낼 세션 이름
  pop_title = add_label(pop, "", &font_ko_14, COLOR_ACCENT);
  lv_label_set_long_mode(pop_title, LV_LABEL_LONG_DOT);
  lv_obj_align(pop_title, LV_ALIGN_TOP_LEFT, 0, 2);
  // 단축키 화면의 키워드 (단축키 설정과 같은 목록), 그 아래 키
  pop_list = lv_obj_create(pop);
  lv_obj_remove_style_all(pop_list);
  lv_obj_set_flex_flow(pop_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(pop_list, 4, 0);
  lv_obj_clear_flag(pop_list, LV_OBJ_FLAG_SCROLLABLE);
  add_pop_drag(pop_list);  // 줄 사이 틈
  for (lv_obj_t *&r : pop_rows) {
    r = lv_obj_create(pop_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, 4, 0);
    lv_obj_set_scroll_dir(r, LV_DIR_HOR);  // 줄마다 옆으로만 밀린다 (아래로 끌면 팝업 닫기)
    lv_obj_set_scrollbar_mode(r, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_GESTURE_BUBBLE);  // 줄 위에서 오른쪽으로 밀어도 뒤로 가지 않는다
    add_pop_drag(r);  // 버튼 사이 틈
  }
  fill_pop();
}

void orcapanel_keywords_changed() {
  if (!pop_list) return;
  for (lv_obj_t *r : pop_rows) lv_obj_clean(r);
  pop_count = 0;
  fill_pop();
  if (area_w > 0) place_pop();
}

// 세션 버튼은 그대로 두고 그 위에 겹친다
static void place_pop() {
  // 3줄: 키워드 두 줄 (둘째 줄 끝에 지우기), 맨 아래는 녹음 / Tab / Tab + Enter / Enter / Esc (fill_pop)
  int rows = 0;
  for (lv_obj_t *r : pop_rows) rows += lv_obj_get_child_cnt(r) > 0;
  int pw = area_w - 8;
  int ph = 12 + 28 + rows * POP_BTN_H + (rows - 1) * 4 + 2;
  lv_obj_set_pos(pop, 4, area_h - ph - 4);
  lv_obj_set_size(pop, pw, ph);
  lv_obj_set_width(pop_title, pw - 12);
  int list_w = pw - 12 - 2;
  lv_obj_set_size(pop_list, list_w, ph - 12 - 2 - 28);
  lv_obj_set_pos(pop_list, 0, 28);
  // 버튼은 글자가 다 보이는 폭 (POP_BTN_MIN_W 보다 좁지 않게). 줄이 남으면 남는 폭을 나눠 줄 끝까지 늘린다.
  // 넘치면 버튼이 POP_FIT_MAX 개 이하인 줄은 글자 폭에 비례해 줄여 한 화면에 넣고 (긴 이름은 끝이 ...),
  // 그보다 많은 줄은 그대로 두어 그 줄만 옆으로 밀어 본다
  for (lv_obj_t *r : pop_rows) {
    int n = lv_obj_get_child_cnt(r);
    if (n == 0) {
      lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(r, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(r, list_w, POP_BTN_H);
    int want[POP_MAX], total = (n - 1) * 4;
    for (int k = 0; k < n; k++) {
      lv_obj_t *b = lv_obj_get_child(r, k);
      // 녹음 버튼은 녹음하는 동안 글자가 길어진다
      const char *text = b == pop_dictate ? "넣는 중..." : (const char *)lv_obj_get_user_data(b);
      want[k] = LV_MAX(lv_txt_get_width(text, strlen(text), &font_ko_14, 0, LV_TEXT_FLAG_NONE) + 16, POP_BTN_MIN_W);
      total += want[k];
    }
    if (!pop_open) lv_obj_scroll_to_x(r, 0, LV_ANIM_OFF);  // 다시 열면 줄 처음부터
    if (total > list_w && n <= POP_FIT_MAX) {
      int gaps = (n - 1) * 4, sum = total - gaps, room = list_w - gaps, acc = 0, used = 0;
      for (int k = 0; k < n; k++) {
        acc += want[k];
        int end = acc * room / sum;  // 앞에서부터 쌓아 나눠서 반올림 오차 없이 줄 끝에 딱 맞춘다
        lv_obj_set_size(lv_obj_get_child(r, k), end - used, POP_BTN_H);
        used = end;
      }
      continue;
    }
    int extra = total < list_w ? list_w - total : 0;
    for (int k = 0; k < n; k++) {
      lv_obj_set_size(lv_obj_get_child(r, k), want[k] + extra / n + (k < extra % n), POP_BTN_H);
    }
  }
  if (pop_open) {
    lv_obj_clear_flag(pop, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(pop, LV_OBJ_FLAG_HIDDEN);
  }
  // 팝업이 떠 있는 동안은 쪽 아래에 팝업만큼 빈 자리를 두어, 가려진 세션 버튼을 위로 올릴 수 있게 한다
  for (Page &p : pages) {
    lv_obj_set_style_pad_bottom(p.page, pop_open ? ph + 8 : 8, 0);
    if (pop_open) {
      lv_obj_add_flag(p.page, LV_OBJ_FLAG_SCROLLABLE);
    } else {
      lv_obj_scroll_to_y(p.page, 0, LV_ANIM_OFF);
      lv_obj_clear_flag(p.page, LV_OBJ_FLAG_SCROLLABLE);
    }
  }
}

// 띄운 세션 버튼이 팝업에 가려지면 그 쪽을 위로 올린다
static void reveal_active() {
  if (!pop_open) return;
  for (Page &p : pages) {
    for (int k = 0; k < PER_PAGE; k++) {
      int i = p.session[k];
      if (i >= 0 && i < count && strcmp(sessions[i].id, active_id) == 0) {
        lv_obj_update_layout(p.page);
        lv_obj_scroll_to_view(p.btn[k], LV_ANIM_ON);
        return;
      }
    }
  }
}

static void set_pop(bool open) {
  if (open != pop_open) {
    pop_open = open;
    // 끌던 중이나 닫히며 내려간 자리에서 다시 열리지 않게
    lv_anim_del(pop, pop_translate);
    lv_obj_set_style_translate_y(pop, 0, 0);
    place_pop();
  }
  reveal_active();
}

// 상태 알림: 지난번 목록과 비교해 새 권한 요청, 작업 중 -> 완료·입력 기다림이면 알림과 소리.
// 처음 받은 목록(켠 직후, ORCA 를 다시 읽기 시작)은 비교할 것이 없어 알리지 않는다. 같은 목록을 다시 받아도(다시 연결) 바뀐 것이 없다.
struct Seen {
  char id[12];
  char state[16];
  uint32_t asked_at, done_at;  // 그 알림을 마지막으로 낸 시각 (0 이면 아직)
};
static Seen seen[MAX_ROWS];
static int seen_count = 0;
static bool seen_ok = false;  // seen 이 ORCA 를 읽은 목록인지

static bool quiet(uint32_t at) {
  return at && lv_tick_elaps(at) < ALERT_QUIET_MS;
}

static void check_alerts(const Session *next, int n) {
  static Seen now[MAX_ROWS];
  const char *asked = nullptr, *done = nullptr;  // 알릴 세션 이름 (처음 것)
  int asked_n = 0, done_n = 0;
  for (int i = 0; i < n; i++) {
    const Session &s = next[i];
    Seen &m = now[i];
    snprintf(m.id, sizeof(m.id), "%s", s.id);
    snprintf(m.state, sizeof(m.state), "%s", s.state);
    m.asked_at = m.done_at = 0;
    int j = 0;
    while (j < seen_count && strcmp(seen[j].id, s.id) != 0) j++;
    const char *before = j < seen_count ? seen[j].state : "";
    if (j < seen_count) m.asked_at = seen[j].asked_at, m.done_at = seen[j].done_at;
    if (!seen_ok) continue;
    if (is_permission(s.state) && !is_permission(before) && !quiet(m.asked_at)) {
      m.asked_at = lv_tick_get() | 1;
      if (!asked_n++) asked = s.title;
    } else if (strcmp(before, "working") == 0 && is_waiting(s.state) && !quiet(m.done_at)) {
      m.done_at = lv_tick_get() | 1;
      if (!done_n++) done = s.title;
    }
  }
  memcpy(seen, now, sizeof(Seen) * n);
  seen_count = n;
  seen_ok = true;
  if (!asked && !done) return;
  // 권한 요청이 먼저 (기다리게 하면 작업이 멈춘다)
  char text[160];
  const char *name = asked ? asked : done;
  int more = (asked ? asked_n : done_n) - 1;
  if (more > 0) {
    snprintf(text, sizeof(text), "%s\n%.60s 외 %d개", asked ? "ORCA 권한 요청" : "ORCA 작업 끝", name, more);
  } else {
    snprintf(text, sizeof(text), "%s\n%.60s", asked ? "ORCA 권한 요청" : "ORCA 작업 끝", name);
  }
  const char *sound = asked ? "limit_warn" : "limit_reset";  // alerts.cpp 의 소리를 같이 쓴다
  Serial.printf("[orca] alert %s\n", sound);
  ui_toast(text, 6000);
  if (speaker_play(sound)) return;
  char event[48];
  snprintf(event, sizeof(event), "{\"sound\":\"%s\"}", sound);
  link_send_event(event);
}

bool orcapanel_received() {
  return received;
}

bool orcapanel_running() {
  return orca_ok;
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
  // ORCA 를 못 읽은 목록(꺼짐)은 비교하지 않는다. 다시 읽기 시작하면 첫 목록은 알리지 않는다.
  if (orca_ok) {
    check_alerts(next, n);
  } else {
    seen_ok = false;
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
    lv_obj_clear_flag(p.page, LV_OBJ_FLAG_SCROLLABLE);  // 팝업이 떠 있을 때만 위아래로 (place_pop)
    lv_obj_set_scroll_dir(p.page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p.page, LV_SCROLLBAR_MODE_OFF);
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
      lv_obj_add_event_cb(p.btn[k], btn_event, LV_EVENT_PRESSED, NULL);
      lv_obj_add_event_cb(p.btn[k], btn_long_event, LV_EVENT_LONG_PRESSED, NULL);
      p.lbl[k] = add_label(p.btn[k], "", &font_ko_14, lv_color_white());
      lv_obj_set_style_text_align(p.lbl[k], LV_TEXT_ALIGN_CENTER, 0);
      lv_label_set_long_mode(p.lbl[k], LV_LABEL_LONG_DOT);
      lv_obj_add_flag(p.btn[k], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(p.page, LV_OBJ_FLAG_HIDDEN);
  }
  build_pop();
  build_qr();
  orcapanel_refresh();
}

void orcapanel_layout(int x, int y, int w, int h) {
  area_w = w, area_h = h;
  lv_obj_set_size(panel, w, h);
  lv_obj_set_pos(panel, x, y);
  lv_obj_set_width(state_lbl, w - 20);
  lv_obj_align(state_lbl, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_size(pager, w, h);
  lv_obj_set_pos(pager, 0, 0);
  place_pop();
  place_qr();
  // 세로 화면은 폭이 좁아 2칸으로 (이름이 ... 으로 잘리지 않게). 홈 아이콘과 같다 (ui.cpp layout_home).
  cols = w > h ? 3 : 2;
  rows = PER_PAGE / cols;
  // 버튼마다 위에 프로젝트 이름 자리를 둔다
  int inner_w = w - 16, inner_h = h - 16 - rows * HEAD_H;
  btn_w = (inner_w - GAP * (cols - 1)) / cols;
  btn_h = (inner_h - GAP * (rows - 1)) / rows;
  label_w = btn_w - 12;
  for (Page &p : pages) {
    lv_obj_set_size(p.page, w, h);
    for (int k = 0; k < PER_PAGE; k++) {
      int x = (k % cols) * (btn_w + GAP), y = (k / cols) * (HEAD_H + btn_h + GAP);
      lv_obj_set_pos(p.head[k], x + 2, y + 1);
      lv_obj_set_width(p.head[k], k == cols - 1 ? btn_w - 34 : btn_w - 2);  // 첫 줄 오른쪽은 쪽 번호 자리
      lv_obj_set_size(p.btn[k], btn_w, btn_h);
      lv_obj_set_pos(p.btn[k], x, y + HEAD_H);
      lv_obj_set_width(p.lbl[k], label_w);
    }
  }
  lay_out_pages();      // 한 줄 칸 수가 바뀌었을 수 있다
  orcapanel_refresh();  // 글자 폭이 바뀌었다
}

void orcapanel_show(bool on) {
  if (on) {
    orcapanel_refresh();
    lv_obj_scroll_to_x(pager, 0, LV_ANIM_OFF);  // 늘 가장 최근 쪽부터
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
  } else {
    // 받아쓰기는 다른 화면으로 가도 이어진다 (사용량 화면 버튼과 같다)
    close_qr();
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  }
}
