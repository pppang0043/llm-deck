#include "pcpanel.h"

#include "link.h"
#include "ui.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define COLOR_BG lv_color_hex(0x101418)
#define COLOR_DIM lv_color_hex(0x8A939E)
#define COLOR_ACCENT lv_color_hex(0xD97757)
#define COLOR_WARN lv_color_hex(0xE0A030)
#define COLOR_FULL lv_color_hex(0xE0483C)
#define COLOR_ROW lv_color_hex(0x1C232B)

// 버튼 하나. PC 도우미가 아는 이름(id)만 보낸다: 프로그램은 companion/launcher.py, 키워드는 companion/typer.py
struct Shortcut {
  const char *id;
  const char *name;    // 버튼에 보이는 이름
  const char *object;  // 알림 글에 쓰는 말 ("엑셀을" 처럼 조사까지)
  const char *letter;  // 프로그램 버튼의 큰 글자 (키워드는 nullptr)
  uint32_t color;
};
static const Shortcut PROGRAMS[] = {
    {"excel", "엑셀", "엑셀을", "X", 0x1D7044},
    {"powerpoint", "파워포인트", "파워포인트를", "P", 0xC43E1C},
    {"orca", "ORCA", "ORCA를", "O", 0x1E6FB8},
};
static const Shortcut KEYWORDS[] = {
    {"commit", "커밋해줘", "\"커밋해줘\"를", nullptr, 0x2E7D5B},
    {"deploy", "배포해줘", "\"배포해줘\"를", nullptr, 0x3A6EA5},
    {"error", "에러 확인", "\"에러 확인\"을", nullptr, 0xB0563A},
    {"orchestration", "/orchestration", "\"/orchestration\"을", nullptr, 0x6A4FB0},
};
#define COUNT(a) (sizeof(a) / sizeof(a[0]))
#define MAX_BUTTONS 6

// 버튼 화면 하나 (프로그램 / 키워드)
struct ButtonPage {
  const char *title;
  const char *note;
  const char *event_key;  // PC 로 보내는 알림 이름: {"launch": id} / {"type": id}
  const char *verb;       // 알림 글: "PC에서 엑셀을 엽니다" / "PC에 \"커밋해줘\"를 입력합니다"
  const Shortcut *items;
  size_t count;
  lv_obj_t *panel, *note_lbl;
  lv_obj_t *btns[MAX_BUTTONS];
};
static ButtonPage programs = {"프로그램", "사용 중인 PC에서 엽니다", "launch", "PC에서 %s 엽니다", PROGRAMS,
                              COUNT(PROGRAMS)};
static ButtonPage keywords = {"키워드", "PC의 커서 자리에 입력 (Enter 없음)", "type", "PC에 %s 입력합니다",
                              KEYWORDS, COUNT(KEYWORDS)};

static lv_obj_t *stat_panel, *stat_host, *stat_age, *stat_none;
struct Meter {
  lv_obj_t *row, *name, *value, *bar;
};
static Meter cpu, ram, disk;
static lv_obj_t *net_lbl, *bat_lbl;
static uint32_t stat_ms = 0;  // 마지막으로 받은 시각
static uint32_t drawn_ms = 0;  // 마지막으로 "몇 초 전" 을 고친 시각

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, color, 0);
  return lbl;
}

static lv_obj_t *add_panel(lv_obj_t *parent, const char *title) {
  lv_obj_t *p = lv_obj_create(parent);
  lv_obj_remove_style_all(p);
  lv_obj_set_style_bg_color(p, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(p, 10, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(add_label(p, title, &font_ko_20, COLOR_ACCENT), LV_ALIGN_TOP_LEFT, 0, 0);
  return p;
}

// ---- 프로그램 / 키워드 버튼 ----

static void button_clicked(lv_event_t *e) {
  ButtonPage *page = (ButtonPage *)lv_event_get_user_data(e);
  lv_obj_t *btn = lv_event_get_target(e);
  size_t i = 0;
  while (i < page->count && page->btns[i] != btn) i++;
  if (i >= page->count) return;
  const Shortcut &s = page->items[i];
  if (link_active() == LINK_NONE || link_active() == LINK_CLOUD) {
    ui_toast("연결된 PC가 없습니다\nPC 도우미를 켜 주세요", 2500);
    return;
  }
  char event[48];
  snprintf(event, sizeof(event), "{\"%s\":\"%s\"}", page->event_key, s.id);
  link_send_event(event);
  char text[80];
  snprintf(text, sizeof(text), page->verb, s.object);
  ui_toast(text, 2000);
}

static void build_buttons(lv_obj_t *parent, ButtonPage &page) {
  page.panel = add_panel(parent, page.title);
  page.note_lbl = add_label(page.panel, page.note, &font_ko_14, COLOR_DIM);
  lv_label_set_long_mode(page.note_lbl, LV_LABEL_LONG_DOT);
  for (size_t i = 0; i < page.count && i < MAX_BUTTONS; i++) {
    const Shortcut &s = page.items[i];
    lv_obj_t *b = lv_btn_create(page.panel);
    lv_obj_set_style_bg_color(b, lv_color_hex(s.color), 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_add_event_cb(b, button_clicked, LV_EVENT_CLICKED, &page);
    if (s.letter) {
      // 자리는 layout_buttons 에서 버튼 크기에 맞춰 정한다 (0번 = 큰 글자, 1번 = 이름)
      add_label(b, s.letter, &lv_font_montserrat_48, lv_color_white());
      add_label(b, s.name, &font_ko_20, lv_color_white());
    } else {
      lv_obj_t *name = add_label(b, s.name, &font_ko_20, lv_color_white());
      lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_width(name, lv_pct(100));
      lv_obj_center(name);
    }
    page.btns[i] = b;
  }
}

// 버튼 배치: 3개 이하는 가로 화면에서 나란히, 세로 화면에서 위아래로. 4개 이상은 2열.
static void layout_buttons(ButtonPage &page, int w, int h) {
  int inner_w = w - 20, inner_h = h - 20;
  lv_obj_set_width(page.note_lbl, inner_w);
  lv_obj_set_pos(page.note_lbl, 0, 28);
  bool wide = w > h;
  int n = page.count;
  int cols = n <= 3 ? (wide ? n : 1) : 2;
  int rows = (n + cols - 1) / cols;
  int top = 52, gap = 8;
  int bw = (inner_w - gap * (cols - 1)) / cols;
  int bh = (inner_h - top - gap * (rows - 1)) / rows;
  if (bh > 150) bh = 150;
  for (int i = 0; i < n; i++) {
    lv_obj_t *b = page.btns[i];
    lv_obj_set_size(b, bw, bh);
    lv_obj_set_pos(b, (i % cols) * (bw + gap), top + (i / cols) * (bh + gap));
    lv_obj_t *first = lv_obj_get_child(b, 0);
    if (page.items[i].letter) {
      lv_obj_t *name = lv_obj_get_child(b, 1);
      if (bh >= 100) {
        // 큰 버튼: 큰 글자 위, 이름 아래
        lv_obj_align(first, LV_ALIGN_CENTER, 0, -12);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, 0);
      } else {
        // 낮은 버튼(세로 화면): 큰 글자 왼쪽, 이름 그 옆
        lv_obj_align(first, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 74, 0);
      }
    } else {
      // 글이 버튼보다 넓으면 작은 글꼴로 (줄이 단어 중간에서 끊기지 않게)
      lv_point_t size;
      lv_txt_get_size(&size, lv_label_get_text(first), &font_ko_20, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      lv_obj_set_style_text_font(first, size.x > bw - 14 ? &font_ko_14 : &font_ko_20, 0);
    }
  }
}

static void show_panel(lv_obj_t *p, bool on) {
  if (on) {
    lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
  }
}

void pcpanel_show_programs(bool on) {
  show_panel(programs.panel, on);
}

void pcpanel_show_keywords(bool on) {
  show_panel(keywords.panel, on);
}

// ---- PC 상태 ----

static Meter add_meter(lv_obj_t *parent, const char *name) {
  Meter m;
  m.row = lv_obj_create(parent);
  lv_obj_remove_style_all(m.row);
  lv_obj_clear_flag(m.row, LV_OBJ_FLAG_SCROLLABLE);
  m.name = add_label(m.row, name, &font_ko_14, COLOR_DIM);
  lv_obj_align(m.name, LV_ALIGN_TOP_LEFT, 0, 0);
  m.value = add_label(m.row, "-", &font_ko_14, lv_color_white());
  lv_obj_align(m.value, LV_ALIGN_TOP_RIGHT, 0, 0);
  m.bar = lv_bar_create(m.row);
  lv_bar_set_range(m.bar, 0, 100);
  lv_obj_set_height(m.bar, 8);
  lv_obj_align(m.bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  return m;
}

static void build_status(lv_obj_t *parent) {
  stat_panel = add_panel(parent, "PC 상태");
  stat_host = add_label(stat_panel, "", &font_ko_14, lv_color_white());
  stat_age = add_label(stat_panel, "", &font_ko_14, COLOR_DIM);
  cpu = add_meter(stat_panel, "CPU");
  ram = add_meter(stat_panel, "메모리");
  disk = add_meter(stat_panel, "디스크");
  net_lbl = add_label(stat_panel, "", &font_ko_14, COLOR_DIM);
  bat_lbl = add_label(stat_panel, "", &font_ko_14, COLOR_DIM);
  stat_none = add_label(stat_panel, "PC 도우미에서 받는 중...", &font_ko_14, COLOR_DIM);
}

void pcpanel_build(lv_obj_t *parent) {
  build_buttons(parent, programs);
  build_buttons(parent, keywords);
  build_status(parent);
}

void pcpanel_layout(int x, int y, int w, int h) {
  for (lv_obj_t *p : {programs.panel, keywords.panel, stat_panel}) {
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
  }
  int inner_w = w - 20;
  layout_buttons(programs, w, h);
  layout_buttons(keywords, w, h);

  // PC 상태: 위에서부터 차례로
  lv_obj_align(stat_host, LV_ALIGN_TOP_RIGHT, 0, 4);
  int yy = 30;
  lv_obj_set_pos(stat_age, 0, yy);
  yy += 22;
  for (Meter *m : {&cpu, &ram, &disk}) {
    lv_obj_set_size(m->row, inner_w, 30);
    lv_obj_set_width(m->bar, inner_w);
    lv_obj_set_pos(m->row, 0, yy);
    yy += 36;
  }
  lv_obj_set_pos(net_lbl, 0, yy);
  yy += 20;
  lv_obj_set_pos(bat_lbl, 0, yy);
  lv_obj_align(stat_none, LV_ALIGN_CENTER, 0, 10);
}

void pcpanel_show_status(bool on) {
  if (on) {
    lv_obj_clear_flag(stat_panel, LV_OBJ_FLAG_HIDDEN);
    drawn_ms = 0;
    pcpanel_loop();
  } else {
    lv_obj_add_flag(stat_panel, LV_OBJ_FLAG_HIDDEN);
  }
}

bool pcpanel_status_visible() {
  return stat_panel && !lv_obj_has_flag(stat_panel, LV_OBJ_FLAG_HIDDEN);
}

static void set_meter(Meter &m, int pct, const char *text) {
  pct = constrain(pct, 0, 100);
  lv_bar_set_value(m.bar, pct, LV_ANIM_OFF);
  lv_color_t c = pct >= 90 ? COLOR_FULL : pct >= 70 ? COLOR_WARN : COLOR_ACCENT;
  lv_obj_set_style_bg_color(m.bar, c, LV_PART_INDICATOR);
  lv_label_set_text(m.value, text);
}

void pcpanel_set_status(JsonObjectConst pc, const char *host) {
  stat_ms = millis();
  char buf[64];
  lv_label_set_text(stat_host, host && host[0] ? host : "");
  snprintf(buf, sizeof(buf), "%d%%", (int)(pc["cpu"] | 0));
  set_meter(cpu, pc["cpu"] | 0, buf);
  snprintf(buf, sizeof(buf), "%d%%  %s", (int)(pc["ram"] | 0), pc["ram_text"] | "");
  set_meter(ram, pc["ram"] | 0, buf);
  snprintf(buf, sizeof(buf), "%d%%  %s", (int)(pc["disk"] | 0), pc["disk_text"] | "");
  set_meter(disk, pc["disk"] | 0, buf);
  snprintf(buf, sizeof(buf), "네트워크  받기 %s  보내기 %s", pc["down"] | "-", pc["up"] | "-");
  lv_label_set_text(net_lbl, buf);
  int bat = pc["battery"] | -1;
  if (bat >= 0) {
    snprintf(buf, sizeof(buf), "배터리  %d%%%s", bat, pc["plugged"] == true ? " (충전기 연결)" : "");
    lv_label_set_text(bat_lbl, buf);
  } else {
    lv_label_set_text(bat_lbl, "");
  }
  lv_obj_add_flag(stat_none, LV_OBJ_FLAG_HIDDEN);
}

void pcpanel_loop() {
  static uint32_t asked_ms = 0;
  if (!pcpanel_status_visible()) return;
  if (drawn_ms && millis() - drawn_ms < 1000) return;
  drawn_ms = millis();
  // 받은 지 오래되었으면 숫자를 흐리게 두고 이유를 알린다
  bool have_pc = link_active() != LINK_NONE && link_active() != LINK_CLOUD;
  uint32_t age = stat_ms ? (millis() - stat_ms) / 1000 : 0;
  if (!have_pc) {
    lv_label_set_text(stat_none, "연결된 PC가 없습니다");
    lv_obj_clear_flag(stat_none, LV_OBJ_FLAG_HIDDEN);
  } else if (!stat_ms || age > 15) {
    lv_label_set_text(stat_none, "PC 도우미에서 받는 중...\n(도우미를 새로 깔았는지 확인)");
    lv_obj_clear_flag(stat_none, LV_OBJ_FLAG_HIDDEN);
  }
  char buf[32];
  if (stat_ms) {
    snprintf(buf, sizeof(buf), "%lu초 전", (unsigned long)age);
    lv_label_set_text(stat_age, buf);
  }
  // 보이는 동안 5초마다 "계속 보내 줘" (PC 는 15초 동안 3초마다 보낸다)
  if (have_pc && (!asked_ms || millis() - asked_ms > 5000)) {
    asked_ms = millis();
    link_send_event("{\"pc_watch\":true}");
  }
}
