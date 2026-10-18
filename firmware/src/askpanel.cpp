#include "askpanel.h"

#include "display.h"
#include "link.h"
#include "ui.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define COLOR_BG lv_color_hex(0x101418)
#define COLOR_DIM lv_color_hex(0x8A939E)
#define COLOR_ACCENT lv_color_hex(0xD97757)
#define COLOR_ROW lv_color_hex(0x1C232B)
#define COLOR_TALK lv_color_hex(0x2E7D5B)
#define COLOR_TALKING lv_color_hex(0xC8372D)
#define TALK_H 56

static lv_obj_t *panel, *state_lbl, *new_btn, *mute_btn, *mute_lbl, *text_box, *q_lbl, *a_lbl, *talk_btn, *talk_lbl;
static bool talking = false;  // 녹음 중 (한 번 눌러 시작, 다시 눌러 끝)
static bool dictating = false;  // 지금 누른 것이 받아쓰기(사용량 화면 버튼)인지
static int dictate_step = DICTATE_IDLE;  // 받아쓰기 진행 (PC 가 알려 준다)
// 답을 PC 스피커로 읽어 줄지. 켤 때마다 음소거(읽지 않음)로 시작한다. PC 음량은 건드리지 않는다.
static bool voice = false;
static bool mic_ok = true;    // PC 마이크를 쓸 수 있는지 (PC 가 알려 준다. 모르면 된다고 본다)
// 준비·듣기·답 받는 중이 된 마지막 시각. PC 가 이만큼 아무 말이 없으면 (도우미가 다시 켜졌거나 멈춤) 표시를 지운다
#define BUSY_TIMEOUT_MS 70000
static uint32_t busy_ms = 0;

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

int askpanel_talk_state() {
  if (talking) return ASK_TALKING;
  if (!pc_connected()) return ASK_NO_PC;
  return mic_ok ? ASK_READY : ASK_NO_MIC;
}

void askpanel_refresh() {
  if (!talk_btn) return;
  int st = askpanel_talk_state();
  lv_obj_set_style_bg_color(talk_btn, st == ASK_TALKING ? COLOR_TALKING : st == ASK_READY ? COLOR_TALK : COLOR_ROW, 0);
  lv_obj_set_style_text_color(talk_lbl, st == ASK_READY || st == ASK_TALKING ? lv_color_white() : COLOR_DIM, 0);
  lv_label_set_text(talk_lbl, st == ASK_TALKING ? LV_SYMBOL_AUDIO "  다 말하면 다시 누르기"
                              : st == ASK_NO_PC ? "PC 연결 없음"
                              : st == ASK_NO_MIC ? "PC 마이크 없음"
                                                 : LV_SYMBOL_AUDIO "  눌러서 말하기");
  ui_refresh_talk();
}

bool askpanel_press(bool dictate) {
  int st = askpanel_talk_state();
  if (st == ASK_NO_PC) ui_toast("연결된 PC가 없습니다\nPC 도우미를 켜 주세요", 2500);
  if (st == ASK_NO_MIC) ui_toast("PC에 쓸 수 있는 마이크가 없습니다", 2500);
  if (st != ASK_READY) return false;
  // 음소거를 풀어 두었으면 시작할 때도 알린다 (PC 가 60초에 스스로 녹음을 끝내도 읽어 주도록)
  link_send_event(dictate          ? "{\"ask\":\"start\",\"mode\":\"dictate\"}"
                  : voice ? "{\"ask\":\"start\",\"voice\":true}"
                          : "{\"ask\":\"start\"}");
  talking = true;
  dictating = dictate;
  // PC 가 마이크를 열 때까지 (처음엔 몇 초 걸린다). 열리면 PC 가 "듣는 중" 으로 바꾼다
  if (dictate) {
    dictate_step = DICTATE_PREPARING;
  } else {
    lv_label_set_text(state_lbl, "마이크 준비 중...");
  }
  busy_ms = millis();
  askpanel_refresh();
  return true;
}

bool askpanel_release() {
  if (!talking) return false;
  link_send_event(voice && !dictating ? "{\"ask\":\"stop\",\"voice\":true}" : "{\"ask\":\"stop\"}");
  talking = false;
  if (dictating) dictate_step = DICTATE_WRITING;
  askpanel_refresh();
  return true;
}

void askpanel_set_mic(bool available) {
  if (available == mic_ok) return;
  mic_ok = available;
  askpanel_refresh();
}

bool askpanel_dictating() {
  return talking && dictating;
}

// 한 번 누르면 듣기 시작, 다시 누르면 끝내고 묻는다
static void talk_event(lv_event_t *e) {
  if (!talking) {
    askpanel_press(false);
  } else if (!dictating) {
    askpanel_release();
  }
}

static void draw_mute() {
  lv_label_set_text(mute_lbl, voice ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
  lv_obj_set_style_bg_color(mute_btn, voice ? COLOR_TALK : COLOR_ROW, 0);
}

static void mute_event(lv_event_t *e) {
  voice = !voice;
  draw_mute();
  // 음소거로 바꾸면 PC 가 읽던 답도 바로 멈춘다
  if (!voice && pc_connected()) link_send_event("{\"ask\":\"mute\"}");
  ui_toast(voice ? "답을 PC 스피커로 읽어 줍니다" : "음소거: 답을 글로만 보여 줍니다", 1500);
}

static void new_event(lv_event_t *e) {
  if (!pc_connected()) {
    ui_toast("연결된 PC가 없습니다\nPC 도우미를 켜 주세요", 2500);
    return;
  }
  link_send_event("{\"ask\":\"new\"}");
  lv_label_set_text(q_lbl, "");
  lv_label_set_text(a_lbl, "");
}

void askpanel_build(lv_obj_t *parent) {
  panel = lv_obj_create(parent);
  lv_obj_remove_style_all(panel);
  lv_obj_set_style_bg_color(panel, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(panel, 10, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(add_label(panel, "질문", &font_ko_20, COLOR_ACCENT), LV_ALIGN_TOP_LEFT, 0, 0);
  state_lbl = add_label(panel, "", &font_ko_14, COLOR_DIM);

  new_btn = lv_btn_create(panel);
  lv_obj_set_size(new_btn, 68, 28);
  lv_obj_set_style_bg_color(new_btn, COLOR_ROW, 0);
  lv_obj_set_style_shadow_width(new_btn, 0, 0);
  lv_obj_set_style_pad_all(new_btn, 0, 0);
  lv_obj_add_event_cb(new_btn, new_event, LV_EVENT_CLICKED, NULL);
  lv_obj_center(add_label(new_btn, "새 대화", &font_ko_14, lv_color_white()));

  // 음소거 (기본). 풀면 답이 다 나온 뒤 PC 가 소리로 읽어 준다.
  mute_btn = lv_btn_create(panel);
  lv_obj_set_size(mute_btn, 40, 28);
  lv_obj_set_style_shadow_width(mute_btn, 0, 0);
  lv_obj_set_style_pad_all(mute_btn, 0, 0);
  lv_obj_add_event_cb(mute_btn, mute_event, LV_EVENT_CLICKED, NULL);
  mute_lbl = add_label(mute_btn, "", &lv_font_montserrat_14, lv_color_white());
  lv_obj_center(mute_lbl);
  draw_mute();

  // 질문과 답. 길면 위아래로 밀어 본다.
  text_box = lv_obj_create(panel);
  lv_obj_remove_style_all(text_box);
  lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(text_box, 6, 0);
  lv_obj_set_scroll_dir(text_box, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(text_box, LV_SCROLLBAR_MODE_AUTO);
  q_lbl = add_label(text_box, "", &font_ko_14, COLOR_DIM);
  lv_label_set_long_mode(q_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(q_lbl, lv_pct(100));
  a_lbl = add_label(text_box, "아래 버튼을 누르고 PC 마이크에 말하세요.\n다 말하면 버튼을 한 번 더 누릅니다.", &font_ko_20,
                    lv_color_white());
  lv_label_set_long_mode(a_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(a_lbl, lv_pct(100));

  talk_btn = lv_btn_create(panel);
  lv_obj_set_style_radius(talk_btn, 14, 0);
  lv_obj_set_style_shadow_width(talk_btn, 0, 0);
  lv_obj_add_event_cb(talk_btn, talk_event, LV_EVENT_CLICKED, NULL);
  talk_lbl = add_label(talk_btn, "", &font_ko_20, lv_color_white());
  lv_obj_center(talk_lbl);
  askpanel_refresh();
}

void askpanel_layout(int x, int y, int w, int h) {
  lv_obj_set_size(panel, w, h);
  lv_obj_set_pos(panel, x, y);
  int inner_w = w - 20, inner_h = h - 20;
  lv_obj_align(state_lbl, LV_ALIGN_TOP_LEFT, 48, 4);
  lv_obj_align(new_btn, LV_ALIGN_TOP_RIGHT, 0, -2);
  lv_obj_align_to(mute_btn, new_btn, LV_ALIGN_OUT_LEFT_MID, -6, 0);
  // 상태 글자는 제목과 음소거 버튼 사이에 (좁으면 끝을 줄인다)
  lv_label_set_long_mode(state_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(state_lbl, inner_w - 48 - 68 - 6 - 40 - 6);
  lv_obj_set_size(text_box, inner_w, inner_h - 34 - TALK_H - 8);
  lv_obj_set_pos(text_box, 0, 34);
  lv_obj_set_size(talk_btn, inner_w, TALK_H);
  lv_obj_set_pos(talk_btn, 0, inner_h - TALK_H);
}

void askpanel_show(bool on) {
  if (on) {
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
  } else {
    // 듣는 중에 다른 화면으로 가면 녹음을 버린다
    if (talking && !dictating) {
      link_send_event("{\"ask\":\"cancel\"}");
      talking = false;
      askpanel_refresh();
    }
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  }
}

int askpanel_dictate_step() {
  return dictate_step;
}

void askpanel_set(JsonObjectConst ask) {
  const char *state = ask["state"] | "";
  // 받아쓰기는 질문 화면을 건드리지 않고 사용량 화면 버튼에만 보여 준다 (결과는 PC 가 알림으로 보낸다)
  // PC 가 녹음을 스스로 끝냈으면 (60초 넘음, 마이크 오류) 버튼도 듣는 중을 푼다
  bool recording = strcmp(state, "preparing") == 0 || strcmp(state, "listening") == 0;
  if (talking && !recording) {
    talking = false;
    askpanel_refresh();
  }
  if (strcmp(ask["mode"] | "ask", "dictate") == 0) {
    busy_ms = (ask["done"] | false) ? 0 : millis();
    dictate_step = strcmp(state, "preparing") == 0   ? DICTATE_PREPARING
                   : strcmp(state, "listening") == 0 ? DICTATE_LISTENING
                   : strcmp(state, "thinking") == 0  ? DICTATE_WRITING
                                                     : DICTATE_IDLE;
    ui_refresh_talk();
    return;
  }
  busy_ms = (ask["done"] | false) ? 0 : millis();
  if (strcmp(state, "preparing") == 0) {
    lv_label_set_text(state_lbl, "마이크 준비 중...");
  } else if (strcmp(state, "listening") == 0) {
    lv_label_set_text(state_lbl, "듣는 중... 말하세요");
  } else if (strcmp(state, "thinking") == 0) {
    lv_label_set_text(state_lbl, "답 받는 중...");
  } else {
    lv_label_set_text(state_lbl, "");
  }
  // 준비·듣는 중에는 앞 질문·답을 그대로 둔다 (말을 다 하면 새 질문으로 바뀐다)
  if (strcmp(state, "listening") != 0 && strcmp(state, "preparing") != 0) {
    lv_label_set_text(q_lbl, ask["q"] | "");
    lv_label_set_text(a_lbl, ask["a"] | "");
    lv_obj_set_style_text_color(a_lbl, strcmp(state, "error") == 0 ? COLOR_ACCENT : lv_color_white(), 0);
    // 답이 나오는 동안은 끝을 따라가고, 다 나오면 질문부터 읽게 맨 위로 둔다
    if (ask["done"] | false) {
      lv_obj_scroll_to_y(text_box, 0, LV_ANIM_OFF);
    } else {
      lv_obj_update_layout(text_box);
      lv_obj_scroll_to_y(text_box, LV_COORD_MAX, LV_ANIM_OFF);
    }
  }
  // 질문 화면이 아닐 때 답이 오면 화면을 깨우고 알린다
  display_poke();
  if (lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN) && (ask["done"] | false) && strcmp(state, "done") == 0) {
    ui_toast("질문의 답이 왔습니다", 2000);
  }
}

void askpanel_loop() {
  if (!busy_ms || millis() - busy_ms < BUSY_TIMEOUT_MS) return;
  // PC 가 70초 동안 아무것도 안 보냈다. 듣는 중이었으면 (PC 는 60초에 스스로 끝낸다) 끝을 보내 본다
  if (talking) askpanel_release();
  busy_ms = 0;
  if (dictate_step != DICTATE_IDLE) {
    dictate_step = DICTATE_IDLE;
    ui_refresh_talk();
    ui_toast("PC에서 받아쓰기 결과가 오지 않았습니다", 2500);
    return;
  }
  lv_label_set_text(state_lbl, "");
  lv_label_set_text(a_lbl, "PC에서 답이 오지 않았습니다\n다시 눌러 주세요");
  lv_obj_set_style_text_color(a_lbl, COLOR_ACCENT, 0);
}
