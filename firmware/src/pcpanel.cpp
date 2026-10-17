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

// PC 도우미(companion/launcher.py)가 아는 이름만 보낸다
struct Shortcut {
  const char *id;
  const char *name;
  const char *object;  // "엑셀을" 처럼 조사까지
  const char *letter;
  uint32_t color;
};
static const Shortcut SHORTCUTS[] = {
    {"excel", "엑셀", "엑셀을", "X", 0x1D7044},
    {"powerpoint", "파워포인트", "파워포인트를", "P", 0xC43E1C},
};
#define SHORTCUT_COUNT (sizeof(SHORTCUTS) / sizeof(SHORTCUTS[0]))

static lv_obj_t *short_panel, *short_note;
static lv_obj_t *short_btns[SHORTCUT_COUNT];

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

// ---- 단축 버튼 ----

static void shortcut_clicked(lv_event_t *e) {
  const Shortcut &s = SHORTCUTS[(intptr_t)lv_event_get_user_data(e)];
  if (link_active() == LINK_NONE || link_active() == LINK_CLOUD) {
    ui_toast("연결된 PC가 없습니다\nPC 도우미를 켜 주세요", 2500);
    return;
  }
  char event[48];
  snprintf(event, sizeof(event), "{\"launch\":\"%s\"}", s.id);
  link_send_event(event);
  char text[64];
  snprintf(text, sizeof(text), "PC에서 %s 엽니다", s.object);
  ui_toast(text, 2000);
}

static void build_shortcuts(lv_obj_t *parent) {
  short_panel = add_panel(parent, "단축 버튼");
  short_note = add_label(short_panel, "사용 중인 PC에서 엽니다", &font_ko_14, COLOR_DIM);
  for (size_t i = 0; i < SHORTCUT_COUNT; i++) {
    const Shortcut &s = SHORTCUTS[i];
    lv_obj_t *b = lv_btn_create(short_panel);
    lv_obj_set_style_bg_color(b, lv_color_hex(s.color), 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, shortcut_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *letter = add_label(b, s.letter, &lv_font_montserrat_48, lv_color_white());
    lv_obj_align(letter, LV_ALIGN_CENTER, 0, -12);
    lv_obj_t *name = add_label(b, s.name, &font_ko_20, lv_color_white());
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, 0);
    short_btns[i] = b;
  }
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
  for (lv_obj_t *p : {short_panel, stat_panel}) {
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
  }
  int inner_w = w - 20, inner_h = h - 20;

  // 단축 버튼: 제목 아래에 나란히 (세로 화면이면 위아래로)
  lv_obj_set_pos(short_note, 0, 28);
  bool wide = w > h;
  int top = 52;
  int bw = wide ? (inner_w - 10) / 2 : inner_w;
  int bh = wide ? inner_h - top : (inner_h - top - 10) / 2;
  if (bh > 150) bh = 150;
  for (size_t i = 0; i < SHORTCUT_COUNT; i++) {
    lv_obj_set_size(short_btns[i], bw, bh);
    lv_obj_set_pos(short_btns[i], wide ? i * (bw + 10) : 0, wide ? top : top + i * (bh + 10));
  }

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

void pcpanel_show_shortcuts(bool on) {
  if (on) {
    lv_obj_clear_flag(short_panel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(short_panel, LV_OBJ_FLAG_HIDDEN);
  }
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
