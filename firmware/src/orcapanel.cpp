#include "orcapanel.h"

#include <Preferences.h>
#include "psram_json.h"

#include "askpanel.h"
#include "camera_ui.h"
#include "completion_inbox.h"
#include "gallery.h"
#include "link.h"
#include "pcpanel.h"
#include "alerts.h"
#include "ui.h"
#include "quickpanel.h"
#include "mirror.h"
#include "orca_pop.h"
#include "usb_drive.h"

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
#define POP_BTN_MIN_W 52  // 버튼이 이보다 좁아지지 않는다. 줄에 다 안 들어가면 그 줄을 옆으로 밀어 본다
#define POP_FIT_MAX 5     // 버튼이 이만큼 이하인 줄은 밀지 않고 한 화면에 다 넣는다 (넘치면 버튼을 줄인다)
// 위치 조정: 줄이 넘치면 옆으로 밀지 않고 접어 모든 자리가 보인다. 버튼 16개가 어떻게 놓여도 화면 안에 든다
// (가로 320x208: 5칸, 가장 높을 때 202px. 세로 240x288: 3칸, 262px)
#define POP_MOVE_CELL_MIN 54
#define POP_MOVE_BTN_H 26
#define POP_MOVE_EMPTY_H 16  // 빈 줄. 놓을 줄은 가장 가까운 줄이라 낮아도 잘 들어간다
#define POP_DRAG_START 10  // 이만큼 내려야 끌기 (그 전에는 버튼 누르기)
#define POP_DRAG_CLOSE 40  // 놓을 때 이만큼 내렸으면 닫는다
#define ACTIVE_HOLD_MS 3000  // 탭 저장 지연 동안 PC 의 예전 선택으로 되돌아가지 않는다
#define KEEP_ORDER_MS 15000  // 누른 뒤 이만큼은 순서를 바꾸지 않는다 (누르려던 자리에 다른 세션이 오지 않게)
#define ALERT_QUIET_MS 60000  // 한 세션의 같은 알림은 이만큼 안에 다시 내지 않는다 (상태가 오락가락할 때)
// 권한 요청은 허용한 뒤 곧바로 다음 명령을 또 묻는 일이 흔해 길게 막지 않는다. 그 사이에도 아래 띠는 떠 있다
#define ASK_QUIET_MS 5000
#define ASK_STRIP_H 34  // 권한 요청이 있는 동안 화면 아래에 남는 띠

struct Session {
  char id[12];
  char title[96];
  char project[64];
  char state[16];
  char summary[128];  // 끝난 세션의 마지막 답변 요약 (companion/orca.py 의 _summary)
  bool question;  // 사용자에게 질문하는 중 (state 는 waiting 그대로. companion/orca.py 의 "q")
};
static Session *sessions;  // 내부 RAM 을 아끼려고 PSRAM 에 둔다 (orcapanel_build)
static int count = 0;
static char active_id[12] = "";  // 이 기기에서 마지막으로 띄운 세션 (한 번 더 누르면 받아쓰기)
// PC 화면 강조와 팝업/받아쓰기 대상을 구분한다. PC 가 바뀌었다고 입력을 보내지 않는다.
static char shown_id[12] = "", pc_shown_id[12] = "";
static bool panel_visible = false, selection_held = false;
static uint32_t selected_at = 0;
static void sync_pc_selection(bool reveal = false);
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
// 아래 팝업: 띄운 세션(active_id)에 보낼 키워드와 키. 닫으면 다음에 세션을 띄울 때 다시 뜬다.
static lv_obj_t *pop, *pop_title, *pop_list;
static lv_obj_t *pop_rows[POP_ROWS];  // 줄마다 따로 옆으로 밀린다
static lv_obj_t *pop_dictate;  // 녹음 버튼 (받아쓰는 중이면 빨갛게, 글자가 바뀐다)
static int pop_count = 0;
static bool pop_open = false;
static int area_w, area_h;  // 마지막 배치 (팝업 자리를 정한다)
static void set_pop(bool open);
// 팝업 버튼 표 (내부 RAM 을 아끼려고 PSRAM 에 둔다). 버튼의 user_data 가 pop_tab 안을 가리키므로 버튼을 먼저 지우고 바꾼다
static PopTable *pop_tab, *pop_next;
static bool pop_from_pc = false;     // pop_tab 이 PC 가 보낸 표다 (아니면 기본 배치)
static bool pop_next_ready = false;  // pop_next 를 아직 적용하지 않았다 (꾹 누른 창, 위치 조정이 끝난 뒤에)
static bool pop_kw_dirty = false;    // 위 동안 키워드가 바뀌었다
static bool pop_moved = false;       // 위치 조정을 PC 로 보내 버튼이 pop_tab 과 다르다. 다음 pop 은 같아도 적용한다
// 꾹 누른 창 (수정 / 위치 조정 / 취소)
static lv_obj_t *pe, *pe_title;
static char pe_id[sizeof(PopItem::id)] = "";  // 비어 있으면 닫혀 있다
static bool pop_long = false;  // 꾹 누른 뒤 손을 떼면 오는 CLICKED 는 보내지 않는다
// 위치 조정: 제목 자리의 안내와 완료 / 취소, 손가락을 따라오는 버튼 모양, 들어갈 자리 표시
static lv_obj_t *pop_shield;  // 뒤의 세션 버튼과 화면을 막는다
static lv_obj_t *move_bar, *move_ghost, *move_ghost_lbl, *move_mark;
static lv_obj_t *move_btn;  // 끌고 있는 버튼
static bool pop_mode = false;
static void pop_mode_end(bool restore);
static void close_pe();
static void pop_edit_cancel();
static void pop_apply_next();
// 스킬 을 누르면 팝업 바로 위에 뜨는 스킬 고르기. 누르면 "/이름 " 만 넣고 (Enter 없이) 뒤에 할 일을 이어 적는다.
// 이름은 companion/orca.py 의 SKILLS 에도 있어야 PC 가 입력한다
static const char *const SKILLS[] = {"proto-design", "minipc"};
static lv_obj_t *sk, *sk_row;
static bool sk_open = false;
static void place_pop();
// 빠른 답 창: 세션을 길게 누르면 가운데에 뜬다. 권한 요청은 허용 / 거부, 입력 기다림·완료는 계속. 숨기기는 늘.
static lv_obj_t *qr, *qr_title, *qr_btn[3], *qr_lbl[3];
static char qr_id[12] = "";  // 빠른 답을 보낼 세션 (비어 있으면 닫혀 있다)
static bool qr_permission = false;
static bool qr_tapped = false;  // 권한 요청 세션을 눌러 열었다: 숨기기 대신 보기 (세션을 띄운다)
static bool long_pressed = false;  // 길게 누른 뒤 손을 떼면 오는 CLICKED 는 무시한다
static lv_obj_t *ask_strip, *ask_strip_lbl;
static void close_qr();
static void place_qr();

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

// 받아쓰기: 그 세션에 받아 적기 시작, 받아쓰는 중이면 끝내고 넣는다 (세션 버튼을 다시 누르기)
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

// 그 세션을 ORCA 에서 앞으로 띄우고 키 팝업을 연다
static void activate_session(const char *id) {
  char json[64];
  snprintf(json, sizeof(json), "{\"orca\":\"activate\",\"id\":\"%s\"}", id);
  link_send_event(json);
  snprintf(active_id, sizeof(active_id), "%s", id);
  snprintf(shown_id, sizeof(shown_id), "%s", id);
  selected_at = lv_tick_get();
  selection_held = true;
  set_pop(true);
  orcapanel_refresh();
}

// 빠른 답 창을 연다. 권한 요청은 허용 / 거부, 입력 기다림·완료는 계속.
// tapped 이면 마지막 버튼이 보기, 길게 눌렀으면 숨기기.
static void open_qr(const Session &s, bool tapped) {
  set_pop(false);  // 아래에 빠른 답 창이 남으면 창 밖을 눌렀을 때 다른 세션으로 키가 간다
  snprintf(qr_id, sizeof(qr_id), "%s", s.id);
  qr_tapped = tapped;
  qr_permission = is_permission(s.state);
  bool answer = qr_permission || is_waiting(s.state);  // 빠른 답은 권한 요청이나 입력 기다림일 때만
  lv_label_set_text(qr_title, s.title);
  lv_label_set_text(qr_lbl[0], qr_permission ? "허용" : "계속");
  lv_label_set_text(qr_lbl[1], "거부");
  lv_label_set_text(qr_lbl[2], tapped ? "보기" : "숨기기");
  if (answer) lv_obj_clear_flag(qr_btn[0], LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(qr_btn[0], LV_OBJ_FLAG_HIDDEN);
  if (qr_permission) lv_obj_clear_flag(qr_btn[1], LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(qr_btn[1], LV_OBJ_FLAG_HIDDEN);
  place_qr();
  lv_obj_clear_flag(qr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(qr);
}

// 처음 누르면 그 세션을 ORCA 에서 앞으로 띄우기만 한다. 띄운 세션을 다시 누르면 받아쓰기, 또 누르면 넣는다.
// 권한 요청 중인 세션은 PC 창을 건드리지 않고 바로 허용 / 거부 창을 연다.
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
  if (!askpanel_dictating() && (is_permission(s.state) || strcmp(active_id, s.id) != 0)) {
    if (!link_pc_connected()) {
      ui_toast("연결된 PC가 없습니다", 2000);
      return;
    }
    if (is_permission(s.state)) {
      open_qr(s, true);
    } else {
      close_qr();
      activate_session(s.id);
    }
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
  if (!link_pc_connected()) {
    ui_toast("연결된 PC가 없습니다", 2000);
    return;
  }
  open_qr(s, false);
}

// 빠른 답 버튼: 이름만 보낸다. 보낼 키와 지금 상태 확인은 PC 가 한다 (orca.py 의 REPLIES)
static void qr_event(lv_event_t *e) {
  int k = (intptr_t)lv_event_get_user_data(e);
  if (!qr_id[0]) return;
  if (!link_pc_connected()) {
    ui_toast("연결된 PC가 없습니다", 2000);
    return;
  }
  touched_at = lv_tick_get();
  if (k == 2 && qr_tapped) {
    activate_session(qr_id);
    close_qr();
    return;
  }
  char json[96];
  if (k == 2) {
    // 숨기기: PC 가 그 터미널을 닫을 때까지 목록에서 뺀다 (orca.py 의 hide)
    snprintf(json, sizeof(json), "{\"orca\":\"hide\",\"id\":\"%s\"}", qr_id);
  } else {
    const char *reply = !qr_permission ? "continue" : k == 0 ? "allow" : "deny";
    snprintf(json, sizeof(json), "{\"orca\":\"reply\",\"id\":\"%s\",\"reply\":\"%s\"}", qr_id, reply);
  }
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
  static const uint32_t QR_COLORS[3] = {0x2E8B5E, 0xD8433A, 0x4A5563};
  for (int k = 0; k < 3; k++) {
    qr_btn[k] = lv_btn_create(qr);
    lv_obj_set_style_radius(qr_btn[k], 8, 0);
    lv_obj_set_style_shadow_width(qr_btn[k], 0, 0);
    lv_obj_set_style_bg_color(qr_btn[k], lv_color_hex(QR_COLORS[k]), 0);
    lv_obj_add_event_cb(qr_btn[k], qr_event, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    qr_lbl[k] = add_label(qr_btn[k], "", &font_ko_20, lv_color_white());
    lv_obj_center(qr_lbl[k]);
  }
}

// 가운데에 뜬다. 보이는 버튼들이 폭을 나눠 쓴다
static void place_qr() {
  int w = LV_MIN(area_w - 24, 320), h = 112;
  lv_obj_set_size(qr, w, h);
  lv_obj_align(qr, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_width(qr_title, w - 16 - 28);
  int shown = 0;
  for (int k = 0; k < 3; k++)
    if (!lv_obj_has_flag(qr_btn[k], LV_OBJ_FLAG_HIDDEN)) shown++;
  int bw = (w - 16 - 8 * (shown - 1)) / shown, bh = h - 16 - 32, x = 0;
  for (int k = 0; k < 3; k++) {
    if (lv_obj_has_flag(qr_btn[k], LV_OBJ_FLAG_HIDDEN)) continue;
    lv_obj_set_size(qr_btn[k], bw, bh);
    lv_obj_set_pos(qr_btn[k], x, 32);
    x += bw + 8;
  }
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
  const char *problem = !link_pc_connected() ? "PC 연결 없음"
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
    bool mine = askpanel_dictate_target()[0] == '\0';  // 세션에 넣는 받아쓰기가 아니다
    int step = askpanel_dictate_step();
    const PopItem *item = (const PopItem *)lv_obj_get_user_data(pop_dictate);  // 고친 표시명
    lv_label_set_text(lv_obj_get_child(pop_dictate, 0), !mine || step == DICTATE_IDLE ? item->name
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
      bool active = strcmp(shown_id, s.id) == 0;
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
  if (code == LV_EVENT_PRESSED) pop_long = false;
  if (pop_mode) return;  // 위치 조정 중에는 아래로 끌어 닫지 않는다
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
    if (!dragged && sk_open) {  // 스킬 고르기는 따라 내려가지 않으니 먼저 닫는다
      sk_open = false;
      lv_obj_add_flag(sk, LV_OBJ_FLAG_HIDDEN);
    }
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
  if (pop_mode || pop_long || pe_id[0]) return;  // 위치 조정 중이거나 꾹 누른 뒤 손을 뗀 것: 키를 보내지 않는다
  if (dragged) return;  // 끌어서 닫는 중이었다
  const PopItem *item = (const PopItem *)lv_obj_get_user_data(lv_event_get_current_target(e));
  const char *key = item->id;
  if (strcmp(key, "skills") == 0) {  // 스킬 고르기를 열고 닫는다 (PC 로는 보내지 않는다)
    sk_open = !sk_open;
    place_pop();
    return;
  }
  if (!link_pc_connected()) {
    ui_toast("연결된 PC가 없습니다", 2000);
    return;
  }
  touched_at = lv_tick_get();
  if (strcmp(key, "dictate") == 0) {
    // 녹음 버튼은 세션과 상관없는 그냥 받아쓰기: PC 에서 커서가 있는 곳(미러링 창에서 눌렀으면 그 직전 창)에 넣는다.
    // 팝업은 그대로: 받아 적은 글이 들어가면 이어서 Enter 를 누른다
    if (askpanel_dictating()) {
      askpanel_release();
    } else {
      askpanel_press(true);
    }
    orcapanel_refresh();
    return;
  }
  char json[96];
  snprintf(json, sizeof(json), "{\"orca\":\"send\",\"id\":\"%s\",\"key\":\"%s\"}", active_id, key);
  link_send_event(json);
  // 보낸 뒤에도 팝업은 둔다 (Enter 가 함께 나가는 키도. 이어서 누르도록). 닫기는 끌어 내리기로 한다
}

// 꾹 누르면 수정 / 위치 조정 / 취소. 손을 떼기 전에 뜨므로 손을 뗄 때까지 아무것도 누르지 않게 한다
static void pop_long_event(lv_event_t *e) {
  if (pop_mode || dragged || pe_id[0]) return;
  pop_long = true;
  lv_indev_wait_release(lv_indev_get_act());
  touched_at = lv_tick_get();
  if (!link_pc_connected()) {
    ui_toast("연결된 PC가 없습니다", 2000);
    return;
  }
  const PopItem *item = (const PopItem *)lv_obj_get_user_data(lv_event_get_current_target(e));
  sk_open = false;
  place_pop();
  snprintf(pe_id, sizeof(pe_id), "%s", item->id);
  lv_label_set_text(pe_title, item->name);
  lv_obj_clear_flag(pop_shield, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(pop_shield);
  lv_obj_clear_flag(pe, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(pe);
}

static void pop_area(lv_obj_t *obj, PopRect *out) {
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  *out = {a.x1, a.y1, a.x2, a.y2};
}

// 손가락 자리의 줄과 그 줄에서의 순서 (끄는 버튼은 빼고 센다). at 은 들어갈 자리 표시의 왼쪽 위 (화면 좌표)
static void pop_find_slot(const lv_point_t &p, int *row, int *index, lv_point_t *at) {
  PopRect areas[POP_ROWS], btns[POP_MAX];
  for (int r = 0; r < POP_ROWS; r++) pop_area(pop_rows[r], &areas[r]);
  *row = pop_drop_row(areas, POP_ROWS, p.y);
  lv_obj_t *r = pop_rows[*row];
  int n = 0, cnt = lv_obj_get_child_cnt(r);
  for (int k = 0; k < cnt; k++) {
    lv_obj_t *b = lv_obj_get_child(r, k);
    if (b != move_btn) pop_area(b, &btns[n++]);
  }
  *index = pop_drop_index(btns, n, p.x, p.y);
  if (*index < n) {
    *at = {(lv_coord_t)(btns[*index].x1 - 4), (lv_coord_t)btns[*index].y1};
  } else if (n) {
    *at = {(lv_coord_t)(btns[n - 1].x2 + 1), (lv_coord_t)btns[n - 1].y1};
  } else {
    *at = {(lv_coord_t)areas[*row].x1, (lv_coord_t)areas[*row].y1};
  }
}

// 팝업 안의 자리 (화면 좌표에서 빼고, 팝업 안으로 맞춘다)
static void move_to(lv_obj_t *obj, int x, int y) {
  lv_area_t a;
  lv_obj_get_content_coords(pop, &a);
  int w = lv_obj_get_width(obj), h = lv_obj_get_height(obj);
  lv_obj_set_pos(obj, LV_CLAMP(0, x - a.x1, lv_area_get_width(&a) - w), LV_CLAMP(0, y - a.y1, lv_area_get_height(&a) - h));
}

static void move_show(const lv_point_t &p) {
  move_to(move_ghost, p.x - lv_obj_get_width(move_ghost) / 2, p.y - lv_obj_get_height(move_ghost) / 2);
  int row, index;
  lv_point_t at;
  pop_find_slot(p, &row, &index, &at);
  move_to(move_mark, at.x, at.y);
}

static void move_stop() {
  if (move_btn) lv_obj_set_style_opa(move_btn, LV_OPA_COVER, 0);
  move_btn = nullptr;
  lv_obj_add_flag(move_ghost, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(move_mark, LV_OBJ_FLAG_HIDDEN);
}

// 위치 조정: 버튼을 누른 채 끌면 그 모양이 따라오고, 놓으면 그 자리로 옮긴다 (보내는 것은 완료를 눌렀을 때)
static void pop_move_event(lv_event_t *e) {
  if (!pop_mode) return;
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_obj_t *b = lv_event_get_current_target(e);
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  if (code == LV_EVENT_PRESSED) {
    const PopItem *item = (const PopItem *)lv_obj_get_user_data(b);
    move_btn = b;
    lv_obj_set_style_opa(b, LV_OPA_40, 0);
    lv_obj_set_size(move_ghost, lv_obj_get_width(b), lv_obj_get_height(b));
    lv_obj_set_style_bg_color(move_ghost, lv_color_hex(pop_color(item->id, item->slash)), 0);
    lv_label_set_text(move_ghost_lbl, item->name);
    lv_obj_clear_flag(move_ghost, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(move_mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(move_mark);
    lv_obj_move_foreground(move_ghost);
    move_show(p);
    return;
  }
  if (b != move_btn) return;
  if (code == LV_EVENT_PRESSING) {
    move_show(p);
    return;
  }
  if (code == LV_EVENT_RELEASED) {
    int row, index;
    lv_point_t at;
    pop_find_slot(p, &row, &index, &at);
    if (lv_obj_get_parent(b) != pop_rows[row]) lv_obj_set_parent(b, pop_rows[row]);
    lv_obj_move_to_index(b, index);
  }
  move_stop();
  place_pop();
}

static void add_pop_btn(int row, const PopItem *item) {
  if (pop_count >= POP_MAX) return;
  lv_obj_t *b = lv_btn_create(pop_rows[row]);
  pop_count++;
  lv_obj_set_user_data(b, (void *)item);  // 원래 이름 (라벨 글자는 넘치면 LVGL 이 "..." 로 바꿔 둔다)
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 2, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(pop_color(item->id, item->slash)), 0);
  lv_obj_add_event_cb(b, pop_event, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(b, pop_long_event, LV_EVENT_LONG_PRESSED, NULL);
  add_pop_drag(b);
  lv_obj_add_event_cb(b, pop_move_event, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(b, pop_move_event, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(b, pop_move_event, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(b, pop_move_event, LV_EVENT_PRESS_LOST, NULL);
  lv_obj_t *lbl = add_label(b, item->name, &font_ko_14, lv_color_white());
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);  // 한 줄로, 넘치면 ...
  lv_obj_set_size(lbl, LV_PCT(100), lv_font_get_line_height(&font_ko_14));
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(lbl);
  if (strcmp(item->id, "dictate") == 0) pop_dictate = b;
}

// 기본 배치 (PC 가 표를 보낸 적이 없을 때): 키워드 (PC 가 보낸 목록이면 그 이름) 는 윗줄에 (orch 만 둘째 줄), 그 아래 키
static void default_pop(PopTable *t) {
  memset(t, 0, sizeof(*t));
  t->version = POP_TABLE_VERSION;
  int n = 0;
  auto add = [&](int row, const char *id, const char *name, bool slash) {
    if (n >= POP_MAX) return;
    PopItem &item = t->item[n++];
    snprintf(item.id, sizeof(item.id), "%s", id);
    snprintf(item.name, sizeof(item.name), "%s", name);
    item.slash = slash;
    t->rows[row]++;
  };
  const char *id, *name;
  uint32_t color;
  // 줄 순서대로 쌓는다
  for (int i = 0; pcpanel_keyword(i, &id, &name, &color); i++) {
    if (strcmp(id, "tab_enter") != 0 && strcmp(id, "orchestration") != 0) add(0, id, name, color == POP_SLASH_COLOR);  // tab_enter 는 아래 키 버튼과 겹친다
  }
  for (int i = 0; pcpanel_keyword(i, &id, &name, &color); i++) {
    if (strcmp(id, "orchestration") == 0) add(1, id, name, color == POP_SLASH_COLOR);
  }
  add(1, "resume", "리슘", true);
  add(1, "skill:proto-design", "proto", true);  // "/proto-design " 만 넣는다 (뒤에 할 일을 이어 적는다)
  // 입력칸에 쓴 글을 지운다 (Ctrl+U)
  add(1, "erase", "지우기", false);
  add(1, "skills", "스킬", false);
  add(2, "dictate", "녹음", false);
  add(2, "tab", "Tab", false);
  add(2, "tab_enter", "Tab+E", false);
  add(2, "enter", "Enter", false);
  add(2, "esc", "Esc", false);
}

static void fill_pop() {
  if (!pop_from_pc) default_pop(pop_tab);
  int at = 0;
  for (int r = 0; r < POP_ROWS; r++) {
    for (int k = 0; k < pop_tab->rows[r]; k++) add_pop_btn(r, &pop_tab->item[at++]);
  }
}

// 스킬 버튼: "/이름 " 을 넣고 스킬 고르기만 닫는다 (팝업은 그대로, /orchestration 처럼)
static void sk_event(lv_event_t *e) {
  const char *skill = (const char *)lv_event_get_user_data(e);
  if (!link_pc_connected()) {
    ui_toast("연결된 PC가 없습니다", 2000);
    return;
  }
  touched_at = lv_tick_get();
  char json[96];
  snprintf(json, sizeof(json), "{\"orca\":\"send\",\"id\":\"%s\",\"key\":\"skill:%s\"}", active_id, skill);
  link_send_event(json);
  sk_open = false;
  place_pop();
}

static void build_sk() {
  sk = lv_obj_create(panel);
  lv_obj_remove_style_all(sk);
  lv_obj_set_style_bg_color(sk, lv_color_hex(0x1C232B), 0);
  lv_obj_set_style_bg_opa(sk, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(sk, 12, 0);
  lv_obj_set_style_pad_all(sk, 6, 0);
  lv_obj_set_style_border_width(sk, 1, 0);
  lv_obj_set_style_border_color(sk, lv_color_hex(0x3A4350), 0);
  lv_obj_set_style_shadow_width(sk, 24, 0);
  lv_obj_set_style_shadow_color(sk, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(sk, LV_OPA_80, 0);
  lv_obj_clear_flag(sk, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(sk, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(add_label(sk, "스킬", &font_ko_14, COLOR_ACCENT), LV_ALIGN_TOP_LEFT, 0, 2);
  lv_obj_t *close = lv_btn_create(sk);
  lv_obj_set_size(close, 24, 22);
  lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(close, 0, 0);
  lv_obj_set_style_pad_all(close, 0, 0);
  lv_obj_set_ext_click_area(close, 8);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_add_event_cb(close, [](lv_event_t *e) { sk_open = false; place_pop(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_center(add_label(close, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, COLOR_DIM));
  sk_row = lv_obj_create(sk);
  lv_obj_remove_style_all(sk_row);
  lv_obj_set_flex_flow(sk_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(sk_row, 4, 0);
  lv_obj_clear_flag(sk_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(sk_row, 0, 28);
  for (const char *skill : SKILLS) {
    lv_obj_t *b = lv_btn_create(sk_row);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x16808C), 0);
    lv_obj_add_event_cb(b, sk_event, LV_EVENT_CLICKED, (void *)skill);
    lv_obj_t *lbl = add_label(b, "", &font_ko_14, lv_color_white());
    lv_label_set_text_fmt(lbl, "/%s", skill);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_size(lbl, LV_PCT(100), lv_font_get_line_height(&font_ko_14));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
  }
}

static void pop_save() {
  Preferences prefs;
  if (prefs.begin("orca_pop")) {
    prefs.putBytes("table", pop_tab, sizeof(PopTable));
    prefs.end();
  }
}

static void pop_load() {
  pop_tab = (PopTable *)ps_calloc(1, sizeof(PopTable));
  pop_next = (PopTable *)ps_calloc(1, sizeof(PopTable));
  Preferences prefs;
  if (prefs.begin("orca_pop", true)) {
    pop_from_pc = prefs.getBytes("table", pop_tab, sizeof(PopTable)) == sizeof(PopTable) &&
                  pop_tab->version == POP_TABLE_VERSION && pop_table_ok(*pop_tab);
    prefs.end();
  }
}

static void pop_clear() {
  for (lv_obj_t *r : pop_rows) lv_obj_clean(r);
  pop_count = 0;
  pop_dictate = nullptr;
}

// 버튼을 표대로 다시 만든다. 누르던 버튼이 지워지면 LVGL 은 손가락 밑의 새 버튼을 누른 것으로 친다:
// 다른 키가 가지 않게 손을 뗄 때까지 아무것도 누르지 않는다
static void pop_rebuild() {
  pop_clear();
  pop_moved = false;
  fill_pop();
  if (area_w > 0) place_pop();
  for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) lv_indev_wait_release(i);
}

static void pop_apply_next() {
  if (pop_next_ready) {
    pop_next_ready = pop_kw_dirty = false;
    bool same = pop_from_pc && memcmp(pop_tab, pop_next, sizeof(PopTable)) == 0;
    if (same && !pop_moved) return;  // 같은 표 (연결할 때마다 온다)
    pop_clear();
    memcpy(pop_tab, pop_next, sizeof(PopTable));
    pop_from_pc = true;
    if (!same) pop_save();
    pop_rebuild();
  } else if (pop_kw_dirty) {
    pop_kw_dirty = false;
    if (!pop_from_pc) pop_rebuild();
  }
}

void orcapanel_set_pop(JsonArrayConst rows) {
  if (!pop_tab) return;
  PopTable next;
  memset(&next, 0, sizeof(next));
  next.version = POP_TABLE_VERSION;
  bool ok = rows.size() == POP_ROWS;
  int n = 0, r = 0;
  for (JsonArrayConst row : rows) {
    if (r >= POP_ROWS) break;
    for (JsonObjectConst b : row) {
      const char *id = b["id"] | "";
      const char *name = b["name"] | "";
      if (n >= POP_MAX || strlen(id) >= sizeof(PopItem::id) || strlen(name) >= sizeof(PopItem::name)) {
        ok = false;
        break;
      }
      PopItem &item = next.item[n++];
      strlcpy(item.id, id, sizeof(item.id));
      strlcpy(item.name, name, sizeof(item.name));
      item.slash = b["s"] | false;
      next.rows[r]++;
    }
    r++;
  }
  if (!ok || !pop_table_ok(next)) {
    Serial.println("[orca] bad pop list from PC ignored");
    return;
  }
  memcpy(pop_next, &next, sizeof(next));
  pop_next_ready = true;
  if (!pop_mode && !pe_id[0]) pop_apply_next();  // 고치는 중이면 끝난 뒤에
}

// 사용자가 고친 표시명, 입력 글, 위치의 원본은 보드다 (NVS "orca_pop"): 어느 PC 에 꽂아도 같다.
// PC 는 연결할 때 {"pop_sync"} 로 읽어 가서 그 글을 입력하고, 합친 표를 {"pop"} 로 돌려준다 (companion/popbuttons.py)
// 입력 글은 saved_item 의 "text" 로 한 번에 보낸다. 블루투스는 이벤트 하나가 약 210바이트라 그보다 길면
// 조각 {"orca":"saved_text","id","t"} 로 먼저 보내고 saved_item 에 전체 바이트 수 "tl" 을 적는다 (PC 는 길이가 다르면 다시 읽는다).
// 짧은 글을 예전처럼 보내야 조각을 모르는 도우미도 그 글을 입력한다. 보냈으면 true
static bool send_saved_item(const char *id, bool ack) {
  String name, text;
  char key[16];
  Preferences prefs;
  if (prefs.begin("orca_pop", true)) {
    pop_nvs_key(key, sizeof(key), 'n', id);
    if (prefs.isKey(key)) name = prefs.getString(key);
    pop_nvs_key(key, sizeof(key), 't', id);
    if (prefs.isKey(key)) text = prefs.getString(key);
    prefs.end();
  }
  if (!ack && name.isEmpty() && text.isEmpty()) return false;
  JsonDocument doc(PSRAM_JSON);
  doc["orca"] = "saved_item";
  doc["id"] = id;
  if (!name.isEmpty()) doc["name"] = name;
  if (!text.isEmpty()) doc["text"] = text;
  if (ack) doc["ack"] = true;
  String json;
  serializeJson(doc, json);
  if (json.length() <= POP_EVENT_MAX) {
    link_send_event(json.c_str());
    return true;
  }
  doc.remove("text");
  doc["tl"] = text.length();
  for (size_t at = 0, n; at < text.length(); at += n) {
    n = pop_text_part(text.c_str(), text.length(), at, POP_TEXT_PART);
    JsonDocument part(PSRAM_JSON);
    part["orca"] = "saved_text";
    part["id"] = id;
    part["t"] = text.substring(at, at + n);
    json = "";
    serializeJson(part, json);
    link_send_event(json.c_str());
  }
  json = "";
  serializeJson(doc, json);
  link_send_event(json.c_str());
  return true;
}

// items: 연결할 때 읽기라면 보낸 saved_item 수 ("n"). PC 는 받은 수와 다르면 다시 읽는다 (Wi-Fi 는 UDP 라 잃을 수 있다)
static void send_saved_layout(int items = -1) {
  char layout[POP_LAYOUT_MAX] = "";
  Preferences prefs;
  if (prefs.begin("orca_pop", true)) {
    if (prefs.isKey("layout")) prefs.getString("layout", layout, sizeof(layout));
    prefs.end();
  }
  char *rest = layout;
  for (int r = 0; layout[0] && r < POP_ROWS; r++) {
    char *end = strchr(rest, ';');
    if (end) *end = 0;
    char json[POP_LAYOUT_MAX + 48];
    snprintf(json, sizeof(json), "{\"orca\":\"saved_row\",\"row\":%d,\"ids\":\"%s\"}", r, rest);
    link_send_event(json);
    if (!end) break;
    rest = end + 1;
  }
  char end[48] = "{\"orca\":\"saved_end\"}";
  if (items >= 0) snprintf(end, sizeof(end), "{\"orca\":\"saved_end\",\"n\":%d}", items);
  link_send_event(end);
}

void orcapanel_send_saved() {
  if (!pop_tab) return;
  PopTable fallback;
  const PopTable *t = pop_tab;
  if (!pop_from_pc) {
    default_pop(&fallback);
    t = &fallback;
  }
  int n = 0, sent = 0;
  for (uint8_t r : t->rows) n += r;
  for (int i = 0; i < n; i++) sent += send_saved_item(t->item[i].id, false);
  // 새로 만든 버튼: 아직 표에 없을 수 있어 따로 적어 둔 것도 보낸다 (PC 는 이 글을 보고 버튼이 있는 줄 안다)
  char custom[POP_LAYOUT_MAX] = "";
  Preferences prefs;
  if (prefs.begin("orca_pop", true)) {
    if (prefs.isKey("custom")) prefs.getString("custom", custom, sizeof(custom));
    prefs.end();
  }
  for (char *id = strtok(custom, ","); id; id = strtok(nullptr, ",")) sent += send_saved_item(id, false);
  send_saved_layout(sent);
}

// PC 미러 창에서 고친 것 [{"id", "name", "text"}] (null 이면 기본값으로) 이나 {"rows": 위치 글} 을 저장하고 저장한 것을 돌려준다
void orcapanel_save_edits(JsonArrayConst list) {
  Preferences prefs;
  if (!prefs.begin("orca_pop")) return;
  char key[16];
  bool layout = false;
  auto put = [&](char kind, const char *id, const char *value, size_t limit) {
    pop_nvs_key(key, sizeof(key), kind, id);
    if (!value) {
      if (prefs.isKey(key)) prefs.remove(key);
    } else if (strlen(value) < limit) {
      prefs.putString(key, value);
    }
  };
  for (JsonObjectConst b : list) {
    const char *rows = b["rows"];
    if (rows && strlen(rows) < POP_LAYOUT_MAX) {
      prefs.putString("layout", rows);
      layout = true;
    }
    const char *id = b["id"] | "";
    if (!pop_id_ok(id)) continue;
    if (b["new"] | false) {  // 새로 만든 버튼
      String custom = prefs.getString("custom", "");
      if (custom.length() + strlen(id) + 2 < POP_LAYOUT_MAX) prefs.putString("custom", custom + id + ",");
    }
    put('n', id, b["name"], sizeof(PopItem::name));
    put('t', id, b["text"], POP_TEXT_MAX);
  }
  prefs.end();
  for (JsonObjectConst b : list) {
    const char *id = b["id"] | "";
    if (pop_id_ok(id)) send_saved_item(id, true);
  }
  if (layout) send_saved_layout();
}

// 끌어 정한 자리를 보드에 저장한다 (PC 없이도). 버튼 순서가 곧 결과다
static bool save_layout() {
  const char *ids[POP_MAX];
  int counts[POP_ROWS], n = 0;
  memset(pop_next, 0, sizeof(PopTable));
  pop_next->version = POP_TABLE_VERSION;
  for (int r = 0; r < POP_ROWS; r++) {
    counts[r] = lv_obj_get_child_cnt(pop_rows[r]);
    pop_next->rows[r] = counts[r];
    for (int k = 0; k < counts[r]; k++) {
      const PopItem *item = (const PopItem *)lv_obj_get_user_data(lv_obj_get_child(pop_rows[r], k));
      pop_next->item[n] = *item;
      ids[n++] = item->id;
    }
  }
  char layout[POP_LAYOUT_MAX];
  if (!pop_layout_text(layout, sizeof(layout), ids, counts)) return false;
  Preferences prefs;
  if (!prefs.begin("orca_pop")) return false;
  bool saved = prefs.putString("layout", layout) > 0;
  prefs.end();
  if (saved) pop_next_ready = true;  // pop_mode_end 가 이 표를 적용한다
  return saved;
}

static void pop_mode_begin() {
  pop_mode = true;
  dragged = false;
  sk_open = false;
  lv_anim_del(pop, pop_translate);
  lv_obj_set_style_translate_y(pop, 0, 0);
  lv_obj_add_flag(pop_title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(move_bar, LV_OBJ_FLAG_HIDDEN);
  for (lv_obj_t *r : pop_rows) {
    lv_obj_scroll_to_x(r, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);  // 줄 옆 밀기를 끈다
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x2A3038), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 6, 0);
    lv_obj_set_style_pad_all(r, 2, 0);
    lv_obj_set_style_pad_row(r, 4, 0);
  }
  lv_obj_clear_flag(pop_shield, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(pop_shield);
  lv_obj_move_foreground(pop);
  place_pop();
}

// restore: 끝내면서 버튼을 표대로 되돌린다
static void pop_mode_end(bool restore) {
  pop_mode = false;
  move_stop();
  lv_obj_add_flag(pop_shield, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(move_bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(pop_title, LV_OBJ_FLAG_HIDDEN);
  for (lv_obj_t *r : pop_rows) {
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  }
  if (restore) {
    pop_rebuild();
  } else {
    pop_moved = true;
    place_pop();
  }
  pop_apply_next();
}

static void close_pe() {
  if (!pe_id[0]) return;
  pe_id[0] = 0;
  lv_obj_add_flag(pe, LV_OBJ_FLAG_HIDDEN);
  if (!pop_mode) lv_obj_add_flag(pop_shield, LV_OBJ_FLAG_HIDDEN);
  pop_apply_next();
}

// 팝업이 닫히거나 화면을 벗어나면 고치던 것을 버린다
static void pop_edit_cancel() {
  close_pe();
  if (pop_mode) pop_mode_end(true);
}

// 꾹 누른 창의 버튼: 0 수정, 1 신규 (이 버튼 뒤에 새 버튼), 2 위치 조정, 3 취소
static void pe_event(lv_event_t *e) {
  int k = (intptr_t)lv_event_get_user_data(e);
  if (!pe_id[0]) return;
  touched_at = lv_tick_get();
  if (k <= 1) {
    if (!link_pc_connected()) {
      ui_toast("연결된 PC가 없습니다", 2000);
    } else {
      // "PC 미러 창에서 수정하세요" 는 받은 PC 가 보낸다
      char json[96];
      snprintf(json, sizeof(json), k ? "{\"orca\":\"new\",\"after\":\"%s\"}" : "{\"orca\":\"edit\",\"key\":\"%s\"}", pe_id);
      if (!link_send_event(json)) ui_toast("PC로 보내지 못했습니다", 2000);
    }
  }
  close_pe();
  if (k == 2) pop_mode_begin();
}

static void build_pop_edit() {
  // 창 밖이나 뒤의 버튼이 눌리지 않게 패널 전체를 막는다. 꾹 누른 창에서는 창 밖을 누르면 취소
  pop_shield = lv_obj_create(panel);
  lv_obj_remove_style_all(pop_shield);
  lv_obj_set_style_bg_color(pop_shield, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(pop_shield, LV_OPA_40, 0);
  lv_obj_clear_flag(pop_shield, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(pop_shield, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(pop_shield, [](lv_event_t *e) { close_pe(); }, LV_EVENT_CLICKED, NULL);

  pe = lv_obj_create(panel);
  lv_obj_remove_style_all(pe);
  lv_obj_set_style_bg_color(pe, lv_color_hex(0x1C232B), 0);
  lv_obj_set_style_bg_opa(pe, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pe, 12, 0);
  lv_obj_set_style_pad_all(pe, 8, 0);
  lv_obj_set_style_border_width(pe, 1, 0);
  lv_obj_set_style_border_color(pe, lv_color_hex(0x3A4350), 0);
  lv_obj_clear_flag(pe, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(pe, LV_OBJ_FLAG_HIDDEN);
  pe_title = add_label(pe, "", &font_ko_14, COLOR_ACCENT);
  lv_label_set_long_mode(pe_title, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(pe_title, 0, 2);
  static const char *const PE_TEXT[4] = {"수정", "신규", "위치 조정", "취소"};
  static const uint32_t PE_COLORS[4] = {0x2E8B5E, 0x3A6EA5, 0x16808C, 0x4A5563};
  for (int k = 0; k < 4; k++) {
    lv_obj_t *b = lv_btn_create(pe);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(PE_COLORS[k]), 0);
    lv_obj_add_event_cb(b, pe_event, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    lv_obj_center(add_label(b, PE_TEXT[k], &font_ko_14, lv_color_white()));
  }

  move_bar = lv_obj_create(pop);
  lv_obj_remove_style_all(move_bar);
  lv_obj_clear_flag(move_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(move_bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(add_label(move_bar, "끌어 옮기세요", &font_ko_14, COLOR_ACCENT), LV_ALIGN_LEFT_MID, 0, 0);
  for (int k = 0; k < 2; k++) {
    lv_obj_t *b = lv_btn_create(move_bar);
    lv_obj_set_size(b, 52, 24);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(k ? 0x4A5563 : 0x2E8B5E), 0);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, k ? 0 : -56, 0);
    lv_obj_center(add_label(b, k ? "취소" : "완료", &font_ko_14, lv_color_white()));
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
      if (!pop_mode) return;
      touched_at = lv_tick_get();
      if (lv_event_get_user_data(e)) {
        pop_mode_end(true);
      } else if (save_layout()) {
        pop_mode_end(false);
        ui_toast("위치를 저장했습니다", 2000);
        if (link_pc_connected()) send_saved_layout();
      } else {
        ui_toast("위치를 저장하지 못했습니다", 3000);  // 옮긴 자리를 버리지 않고 그대로 둔다
      }
    }, LV_EVENT_CLICKED, (void *)(intptr_t)k);
  }

  move_mark = lv_obj_create(pop);
  lv_obj_remove_style_all(move_mark);
  lv_obj_set_size(move_mark, 3, POP_MOVE_BTN_H);
  lv_obj_set_style_bg_color(move_mark, COLOR_ACCENT, 0);
  lv_obj_set_style_bg_opa(move_mark, LV_OPA_COVER, 0);
  lv_obj_clear_flag(move_mark, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(move_mark, LV_OBJ_FLAG_HIDDEN);

  move_ghost = lv_obj_create(pop);
  lv_obj_remove_style_all(move_ghost);
  lv_obj_set_style_bg_opa(move_ghost, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(move_ghost, 8, 0);
  lv_obj_set_style_border_width(move_ghost, 2, 0);
  lv_obj_set_style_border_color(move_ghost, lv_color_white(), 0);
  lv_obj_clear_flag(move_ghost, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(move_ghost, LV_OBJ_FLAG_HIDDEN);
  move_ghost_lbl = add_label(move_ghost, "", &font_ko_14, lv_color_white());
  lv_label_set_long_mode(move_ghost_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_size(move_ghost_lbl, LV_PCT(100), lv_font_get_line_height(&font_ko_14));
  lv_obj_set_style_text_align(move_ghost_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(move_ghost_lbl);
}

// 가운데에 뜬다
static void place_pe() {
  int w = LV_MIN(area_w - 24, 320), h = 96;
  lv_obj_set_size(pe, w, h);
  lv_obj_align(pe, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_width(pe_title, w - 18);
  // 세로 화면은 좁아 "위치 조정" 에 폭을 더 준다
  int room = w - 18 - 3 * 6, side = room * 22 / 100, x = 0;
  for (int k = 0; k < 4; k++) {
    lv_obj_t *b = lv_obj_get_child(pe, k + 1);  // 0 은 제목
    int bw = k == 2 ? room - 3 * side : side;
    lv_obj_set_size(b, bw, h - 18 - 28);
    lv_obj_set_pos(b, x, 28);
    x += bw + 6;
  }
}

// 위치 조정: 3줄을 다 보이고, 넘치는 줄은 접어서 모든 자리에 놓을 수 있게 한다. 옮길 때마다 부른다
static void place_pop_move() {
  int pw = area_w - 8, list_w = pw - 12 - 2, inner = list_w - 4;
  int cols = LV_MAX((inner + 4) / (POP_MOVE_CELL_MIN + 4), 1), cell = (inner - (cols - 1) * 4) / cols;
  int list_h = 0;
  for (lv_obj_t *r : pop_rows) {
    int n = lv_obj_get_child_cnt(r), lines = (n + cols - 1) / cols;
    int rh = n ? lines * POP_MOVE_BTN_H + (lines - 1) * 4 + 4 : POP_MOVE_EMPTY_H;
    lv_obj_clear_flag(r, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(r, list_w, rh);
    for (int k = 0; k < n; k++) lv_obj_set_size(lv_obj_get_child(r, k), cell, POP_MOVE_BTN_H);
    list_h += rh;
  }
  list_h += (POP_ROWS - 1) * 4;
  int ph = 12 + 28 + list_h + 2;
  lv_obj_set_pos(pop, 4, area_h - ph - 4);
  lv_obj_set_size(pop, pw, ph);
  lv_obj_set_size(pop_list, list_w, list_h);
  lv_obj_set_pos(pop_list, 0, 28);
  lv_obj_set_size(move_bar, pw - 14, 26);
  lv_obj_set_pos(move_bar, 0, 0);
  lv_obj_clear_flag(pop, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(sk, LV_OBJ_FLAG_HIDDEN);
}

static void build_pop() {
  pop_load();
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
  build_sk();
  build_pop_edit();
}

void orcapanel_keywords_changed() {
  if (!pop_list) return;
  pop_kw_dirty = true;
  if (!pop_mode && !pe_id[0]) pop_apply_next();
}

// 세션 버튼은 그대로 두고 그 위에 겹친다
static void place_pop() {
  lv_obj_set_size(pop_shield, area_w, area_h);
  place_pe();
  if (pop_mode) {
    place_pop_move();
    return;
  }
  // 기본 3줄: 키워드 두 줄 (둘째 줄: orch, 리슘, proto, 지우기, 스킬), 맨 아래는 녹음 / Tab / Tab+E / Enter / Esc (default_pop)
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
      const char *text = b == pop_dictate ? "넣는 중..." : ((const PopItem *)lv_obj_get_user_data(b))->name;
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
  // 스킬 고르기: 팝업 바로 위에 같은 폭으로, 스킬 버튼은 폭을 똑같이 나눈다
  int sh = 12 + 28 + POP_BTN_H + 2, sn = sizeof(SKILLS) / sizeof(SKILLS[0]);
  lv_obj_set_pos(sk, 4, area_h - ph - 4 - sh - 4);
  lv_obj_set_size(sk, pw, sh);
  lv_obj_set_size(sk_row, list_w, POP_BTN_H);
  for (int k = 0; k < sn; k++) lv_obj_set_size(lv_obj_get_child(sk_row, k), (list_w - (sn - 1) * 4) / sn, POP_BTN_H);
  if (pop_open && sk_open) {
    lv_obj_clear_flag(sk, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sk);
  } else {
    lv_obj_add_flag(sk, LV_OBJ_FLAG_HIDDEN);
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
static void reveal_session(const char *id) {
  if (!panel_visible || !id[0]) return;
  for (Page &p : pages) {
    for (int k = 0; k < PER_PAGE; k++) {
      int i = p.session[k];
      if (i >= 0 && i < count && strcmp(sessions[i].id, id) == 0) {
        lv_obj_update_layout(pager);
        lv_obj_scroll_to_view(p.page, LV_ANIM_ON);
        lv_obj_update_layout(p.page);
        lv_obj_scroll_to_view(p.btn[k], LV_ANIM_ON);
        return;
      }
    }
  }
}

static void set_pop(bool open) {
  if (!open) pop_edit_cancel();
  if (open != pop_open) {
    pop_open = open;
    sk_open = false;  // 다시 열면 스킬 고르기는 닫혀 있다
    // 끌던 중이나 닫히며 내려간 자리에서 다시 열리지 않게
    lv_anim_del(pop, pop_translate);
    lv_obj_set_style_translate_y(pop, 0, 0);
    place_pop();
  }
  if (pop_open) reveal_session(active_id);
}

// 화면이 숨겨졌을 때는 저장만 한다. 보호 시간 뒤에도 새 패킷 없이 저장한 선택을 적용한다.
static void sync_pc_selection(bool reveal) {
  if (!panel_visible || pop_mode || pe_id[0]) return;  // 고치는 동안에는 팝업을 바꾸지 않는다. 끝나면 다음 주기에 맞춘다
  if (selection_held) {
    if (lv_tick_elaps(selected_at) < ACTIVE_HOLD_MS) {
      if (reveal) reveal_session(shown_id);
      return;
    }
    selection_held = false;
  }
  bool changed = strcmp(shown_id, pc_shown_id) != 0;
  if (changed) {
    snprintf(shown_id, sizeof(shown_id), "%s", pc_shown_id);
    int selected = 0;
    while (selected < count && strcmp(sessions[selected].id, shown_id) != 0) selected++;
    if (mirror_active() && selected < count && !qr_id[0]) {
      close_pop();
      snprintf(active_id, sizeof(active_id), "%s", shown_id);
      set_pop(true);
    } else if (pop_open && strcmp(active_id, shown_id) != 0) close_pop();
    orcapanel_refresh();
  }
  if (changed || reveal) reveal_session(shown_id);
}

// 상태 알림: 지난번 목록과 비교해 새 권한 요청이나 질문이면 알림과 소리, 작업 중 -> 완료·입력 기다림이면 알림함에만 남긴다
// (세션이 끝날 때는 ORCA 앱이 알림음을 낸다).
// 처음 받은 목록(켠 직후, ORCA 를 다시 읽기 시작)은 비교할 것이 없어 알리지 않는다. 같은 목록을 다시 받아도(다시 연결) 바뀐 것이 없다.
struct Seen {
  char id[12];
  char state[16];
  bool question;
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
  const char *asked = nullptr;  // 알릴 세션 이름 (처음 것)
  const char *asked_label = "";
  int asked_n = 0;
  for (int i = 0; i < n; i++) {
    const Session &s = next[i];
    Seen &m = now[i];
    snprintf(m.id, sizeof(m.id), "%s", s.id);
    snprintf(m.state, sizeof(m.state), "%s", s.state);
    m.question = s.question;
    m.asked_at = m.done_at = 0;
    int j = 0;
    while (j < seen_count && strcmp(seen[j].id, s.id) != 0) j++;
    const char *before = j < seen_count ? seen[j].state : "";
    if (j < seen_count) m.asked_at = seen[j].asked_at, m.done_at = seen[j].done_at;
    if (!seen_ok) continue;
    bool new_ask = (is_permission(s.state) && !is_permission(before)) || (s.question && !(j < seen_count && seen[j].question));
    if (new_ask && !(m.asked_at && lv_tick_elaps(m.asked_at) < ASK_QUIET_MS)) {
      m.asked_at = lv_tick_get() | 1;
      if (!asked_n++) {
        asked = s.title;
        asked_label = s.question ? "ORCA 질문" : "ORCA 권한 요청";
      }
    }
    if (strcmp(before, "working") == 0 && is_waiting(s.state) && !quiet(m.done_at)) {
      m.done_at = lv_tick_get() | 1;
      char completed[CompletionInbox::TEXT_SIZE];
      snprintf(completed, sizeof(completed), "ORCA-%s%s%s%s%s", s.project, s.project[0] ? "-" : "", s.title, s.summary[0] ? "\n" : "", s.summary);
      quickpanel_notify(completed);
    }
  }
  memcpy(seen, now, sizeof(Seen) * n);
  seen_count = n;
  seen_ok = true;
  if (!asked) return;
  const char *sound = "limit_reset";  // alerts.cpp 의 소리를 같이 쓴다
  Serial.printf("[orca] alert %s\n", sound);
  char text[160];
  if (asked_n > 1) {
    snprintf(text, sizeof(text), "%s\n%.60s 외 %d개", asked_label, asked, asked_n - 1);
  } else {
    snprintf(text, sizeof(text), "%s\n%.60s", asked_label, asked);
  }
  ui_toast(text, 6000);
  alerts_sound(sound);
}

// 권한 요청 띠: 그 세션이 답을 받을 때까지 어느 화면에서나 남아 있다. 누르면 허용 / 거부 창.
static int first_asking(int *total) {
  int first = -1;
  *total = 0;
  if (!orca_ok || !link_pc_connected()) return first;
  for (int i = 0; i < count; i++) {
    if (!is_permission(sessions[i].state)) continue;
    if (first < 0) first = i;
    ++*total;
  }
  return first;
}

static void ask_strip_clicked(lv_event_t *e) {
  int total;
  int i = first_asking(&total);
  if (i < 0) return;
  touched_at = lv_tick_get();
  ui_show("orca");
  open_qr(sessions[i], true);
  reveal_session(sessions[i].id);
}

static void update_ask_strip() {
  int total;
  int i = first_asking(&total);
  // 카메라·갤러리는 조작 버튼이 아래에 있고, 빠른 답 창이나 키 팝업이 떠 있으면 이미 그 세션을 보고 있다.
  // 빠른 설정이 열려 있으면 그 판을 가리지 않는다.
  bool show = i >= 0 && !camera_ui_open() && !gallery_open() && !usb_drive_active() && !quickpanel_is_open() &&
              !(panel_visible && (pop_open || qr_id[0]));
  if (!show) {
    if (ask_strip) lv_obj_add_flag(ask_strip, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (!ask_strip) {
    ask_strip = lv_btn_create(lv_layer_sys());
    lv_obj_remove_style_all(ask_strip);
    lv_obj_clear_flag(ask_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ask_strip, lv_color_hex(0xD8433A), 0);
    lv_obj_set_style_bg_opa(ask_strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ask_strip, 8, 0);
    lv_obj_add_event_cb(ask_strip, ask_strip_clicked, LV_EVENT_CLICKED, NULL);
    ask_strip_lbl = add_label(ask_strip, "", &font_ko_14, lv_color_white());
    lv_label_set_long_mode(ask_strip_lbl, LV_LABEL_LONG_DOT);
    lv_obj_center(ask_strip_lbl);
    lv_obj_add_flag(ask_strip, LV_OBJ_FLAG_HIDDEN);
  }
  // 같은 값을 넣어도 LVGL 은 다시 그린다. 100ms 마다 도는 곳이라 화면 폭이 바뀔 때만 넣는다
  static int placed_w = 0;
  int w = lv_disp_get_hor_res(NULL) - 8;
  if (w != placed_w) {
    placed_w = w;
    lv_obj_set_size(ask_strip, w, ASK_STRIP_H);
    lv_obj_align(ask_strip, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_width(ask_strip_lbl, w - 16);
  }
  char text[120];
  if (total > 1) {
    snprintf(text, sizeof(text), "권한 요청 %d개: %s", total, sessions[i].title);
  } else {
    snprintf(text, sizeof(text), "권한 요청: %s", sessions[i].title);
  }
  if (strcmp(lv_label_get_text(ask_strip_lbl), text) != 0) lv_label_set_text(ask_strip_lbl, text);
  if (lv_obj_has_flag(ask_strip, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_clear_flag(ask_strip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ask_strip);
  }
}

bool orcapanel_received() {
  return received;
}

bool orcapanel_running() {
  return orca_ok;
}

void orcapanel_set(JsonObjectConst orca) {
  bool pc_changed = strcmp(pc_shown_id, orca["a"] | "") != 0;
  received = true;
  orca_ok = orca["ok"] | false;
  snprintf(pc_shown_id, sizeof(pc_shown_id), "%s", orca_ok ? (orca["a"] | "") : "");
  static Session *next = (Session *)ps_malloc(sizeof(Session) * MAX_ROWS);
  int n = 0;
  for (JsonObjectConst item : orca["list"].as<JsonArrayConst>()) {
    if (n >= MAX_ROWS) break;
    Session &s = next[n++];
    snprintf(s.id, sizeof(s.id), "%s", item["id"] | "");
    snprintf(s.title, sizeof(s.title), "%s", item["t"] | "");
    snprintf(s.project, sizeof(s.project), "%s", item["p"] | "");
    snprintf(s.state, sizeof(s.state), "%s", item["s"] | "idle");
    snprintf(s.summary, sizeof(s.summary), "%s", item["m"] | "");
    s.question = item["q"] | false;
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
  // PC에서 실제 선택이 바뀌었을 때만 ORCA 조작 화면으로 간다. 같은 상태 재전송은 닫은 팝업을 다시 열지 않는다.
  if (mirror_active() && orca_ok && pc_changed && pc_shown_id[0]) {
    bool valid = false;
    for (int i = 0; i < count; i++) valid |= strcmp(sessions[i].id, pc_shown_id) == 0;
    if (valid) ui_show("orca");
  }
  sync_pc_selection();
  orcapanel_refresh();
}

void orcapanel_build(lv_obj_t *parent) {
  sessions = (Session *)ps_calloc(MAX_ROWS, sizeof(Session));
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
  lv_timer_create([](lv_timer_t *) {
    sync_pc_selection();
    update_ask_strip();
  }, 100, nullptr);
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
  panel_visible = on;
  if (on) {
    orcapanel_refresh();
    lv_obj_scroll_to_x(pager, 0, LV_ANIM_OFF);  // 늘 가장 최근 쪽부터
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    sync_pc_selection(true);
  } else {
    // 받아쓰기는 다른 화면으로 가도 이어진다 (사용량 화면 버튼과 같다)
    close_qr();
    pop_edit_cancel();
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  }
}
