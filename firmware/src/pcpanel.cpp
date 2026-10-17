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
    {"powershell", "파워쉘", "파워쉘을", ">_", 0x2B5797},
    {"powershell_admin", "파워쉘 (관리자)", "관리자 파워쉘을", ">_", 0x7A2E3B},
};
static const Shortcut KEYWORDS[] = {
    {"commit", "커밋해줘", "\"커밋해줘\"를", nullptr, 0x2E7D5B},
    {"deploy", "배포해줘", "\"배포해줘\"를", nullptr, 0x3A6EA5},
    {"error", "에러 확인", "\"에러 확인\"을", nullptr, 0xB0563A},
    {"orchestration", "/orchestration", "\"/orchestration\"을", nullptr, 0x6A4FB0},
};
#define COUNT(a) (sizeof(a) / sizeof(a[0]))
#define MAX_BUTTONS 6
#define RAIL_W 66  // 왼쪽 끝의 고르기 칸 폭

// 단축키 화면 안의 한 묶음 (프로그램 / 키워드). 왼쪽 끝 칸에서 고른다.
struct ButtonPage {
  const char *title;
  const char *note;
  const char *event_key;  // PC 로 보내는 알림 이름: {"launch": id} / {"type": id}
  const char *verb;       // 알림 글: "PC에서 엑셀을 엽니다" / "PC에 \"커밋해줘\"를 입력합니다"
  const Shortcut *items;
  size_t count;
  lv_obj_t *area, *note_lbl, *tab;
  lv_obj_t *btns[MAX_BUTTONS];
};
static ButtonPage programs = {"프로그램", "사용 중인 PC에서 엽니다", "launch", "PC에서 %s 엽니다", PROGRAMS,
                              COUNT(PROGRAMS)};
static ButtonPage keywords = {"키워드", "PC의 커서 자리에 입력하고 Enter", "type", "PC에 %s 입력합니다",
                              KEYWORDS, COUNT(KEYWORDS)};
static ButtonPage *const PAGES[] = {&programs, &keywords};
static lv_obj_t *short_panel, *rail;
static int selected = 0;
static bool screen_wide = true;

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

// ---- 단축키 (왼쪽 끝 칸에서 프로그램 / 키워드를 고른다) ----

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
  char text[96];
  snprintf(text, sizeof(text), page->verb, s.object);
  // 관리자 권한은 PC 에서 허용을 눌러야 열린다
  if (strcmp(s.id, "powershell_admin") == 0) strlcat(text, "\nPC 화면에서 \"예\"를 눌러 주세요", sizeof(text));
  ui_toast(text, strcmp(s.id, "powershell_admin") == 0 ? 4000 : 2000);
}

static void select_page(int index) {
  selected = index;
  for (int i = 0; i < (int)COUNT(PAGES); i++) {
    ButtonPage *p = PAGES[i];
    bool on = i == index;
    if (on) {
      lv_obj_clear_flag(p->area, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(p->area, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_bg_color(p->tab, on ? COLOR_ACCENT : COLOR_ROW, 0);
  }
}

static void tab_clicked(lv_event_t *e) {
  select_page((intptr_t)lv_event_get_user_data(e));
}

static void build_page(ButtonPage &page, int index) {
  // 왼쪽 끝 칸의 고르기 버튼
  page.tab = lv_btn_create(rail);
  lv_obj_set_style_radius(page.tab, 10, 0);
  lv_obj_set_style_shadow_width(page.tab, 0, 0);
  lv_obj_set_style_pad_all(page.tab, 2, 0);
  lv_obj_add_event_cb(page.tab, tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);
  lv_obj_center(add_label(page.tab, page.title, &font_ko_14, lv_color_white()));

  // 오른쪽: 설명 한 줄과 버튼들
  page.area = lv_obj_create(short_panel);
  lv_obj_remove_style_all(page.area);
  lv_obj_clear_flag(page.area, LV_OBJ_FLAG_SCROLLABLE);
  page.note_lbl = add_label(page.area, page.note, &font_ko_14, COLOR_DIM);
  lv_label_set_long_mode(page.note_lbl, LV_LABEL_LONG_DOT);
  for (size_t i = 0; i < page.count && i < MAX_BUTTONS; i++) {
    const Shortcut &s = page.items[i];
    lv_obj_t *b = lv_btn_create(page.area);
    lv_obj_set_style_bg_color(b, lv_color_hex(s.color), 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_add_event_cb(b, button_clicked, LV_EVENT_CLICKED, &page);
    if (s.letter) {
      // 자리와 글꼴은 layout_page 에서 버튼 크기에 맞춰 정한다 (0번 = 큰 글자, 1번 = 이름)
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

static int text_width(lv_obj_t *label, const lv_font_t *font) {
  lv_point_t size;
  lv_txt_get_size(&size, lv_label_get_text(label), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return size.x;
}

// 버튼 배치: 가로 화면은 3개 이하면 나란히, 많으면 2열. 세로 화면은 한 줄에 하나.
static void layout_page(ButtonPage &page, int x, int w, int h) {
  lv_obj_set_size(page.area, w, h);
  lv_obj_set_pos(page.area, x, 0);
  int inner_w = w - 16, inner_h = h - 16;
  lv_obj_set_style_pad_all(page.area, 8, 0);
  lv_obj_set_width(page.note_lbl, inner_w);
  lv_obj_set_pos(page.note_lbl, 0, 0);
  int n = page.count;
  int cols = screen_wide ? (n <= 3 ? n : 2) : 1;
  int rows = (n + cols - 1) / cols;
  int top = 24, gap = 6;
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
        lv_obj_set_style_text_font(first, &lv_font_montserrat_48, 0);
        lv_obj_align(first, LV_ALIGN_CENTER, 0, -12);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(name, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(name, text_width(name, &font_ko_20) > bw - 12 ? &font_ko_14 : &font_ko_20, 0);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, 0);
      } else {
        // 낮은 버튼: 큰 글자 왼쪽, 이름 그 옆 (높이가 모자라면 큰 글자를 줄인다)
        const lv_font_t *big = bh >= 60 ? &lv_font_montserrat_48 : &lv_font_montserrat_28;
        int offset = 6 + text_width(first, big) + 10;  // 큰 글자 폭만큼 띄운다 (X 와 >_ 는 폭이 다르다)
        lv_obj_set_style_text_font(first, big, 0);
        lv_obj_align(first, LV_ALIGN_LEFT_MID, 6, 0);
        // 이름이 남은 폭보다 길면 작은 글꼴로, 그래도 길면 두 줄로
        int room = bw - offset - 8;
        lv_obj_set_style_text_font(name, text_width(name, &font_ko_20) > room ? &font_ko_14 : &font_ko_20, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(name, room);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, offset, 0);
      }
    } else {
      // 글이 버튼보다 넓으면 작은 글꼴로 (줄이 단어 중간에서 끊기지 않게)
      lv_obj_set_style_text_font(first, text_width(first, &font_ko_20) > bw - 14 ? &font_ko_14 : &font_ko_20, 0);
    }
  }
}

static void build_shortcuts(lv_obj_t *parent) {
  short_panel = lv_obj_create(parent);
  lv_obj_remove_style_all(short_panel);
  lv_obj_set_style_bg_color(short_panel, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(short_panel, LV_OPA_COVER, 0);
  lv_obj_clear_flag(short_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(short_panel, LV_OBJ_FLAG_HIDDEN);
  rail = lv_obj_create(short_panel);
  lv_obj_remove_style_all(rail);
  lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
  for (int i = 0; i < (int)COUNT(PAGES); i++) build_page(*PAGES[i], i);
  select_page(0);
}

static void layout_shortcuts(int x, int y, int w, int h) {
  lv_obj_set_size(short_panel, w, h);
  lv_obj_set_pos(short_panel, x, y);
  screen_wide = w > h;
  // 왼쪽 끝 칸: 고르기 버튼을 위아래로
  lv_obj_set_size(rail, RAIL_W, h);
  lv_obj_set_pos(rail, 0, 0);
  int n = COUNT(PAGES), gap = 6;
  int th = (h - 12 - gap * (n - 1)) / n;
  for (int i = 0; i < n; i++) {
    lv_obj_set_size(PAGES[i]->tab, RAIL_W - 10, th);
    lv_obj_set_pos(PAGES[i]->tab, 6, 6 + i * (th + gap));
  }
  for (ButtonPage *p : PAGES) layout_page(*p, RAIL_W, w - RAIL_W, h);
}

void pcpanel_show_shortcuts(bool on) {
  if (on) {
    lv_obj_clear_flag(short_panel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(short_panel, LV_OBJ_FLAG_HIDDEN);
  }
}

void pcpanel_select_shortcuts(int page) {
  if (page >= 0 && page < (int)COUNT(PAGES)) select_page(page);
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
  build_shortcuts(parent);
  build_status(parent);
}

void pcpanel_layout(int x, int y, int w, int h) {
  layout_shortcuts(x, y, w, h);
  lv_obj_set_size(stat_panel, w, h);
  lv_obj_set_pos(stat_panel, x, y);
  int inner_w = w - 20;

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
