#include "quickpanel.h"
#include "quick_swipe.h"
#include "usb_drive.h"
#include "display.h"
#include "completion_inbox.h"
#include "ui.h"
#include <new>

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

struct QuickPanel {
  lv_disp_t *display = nullptr;
  lv_obj_t *overlay = nullptr, *sheet = nullptr, *handle = nullptr;
  lv_obj_t *title = nullptr, *close = nullptr, *sleep = nullptr, *drive = nullptr, *hint = nullptr;
  lv_obj_t *sleep_name = nullptr, *sleep_detail = nullptr, *drive_name = nullptr, *drive_detail = nullptr;
  lv_obj_t *notices = nullptr, *notice_text = nullptr, *banner = nullptr, *banner_text = nullptr;
  lv_obj_t *status = nullptr, *status_text = nullptr, *status_anchor = nullptr;
  bool tracking = false, header_touch = false;
  bool open = false;
  QuickSwipe swipe;
};
static QuickPanel panels[2];  // 원래 화면과 미러링 중 기기 화면은 서로 다른 LVGL 디스플레이
static void (*on_sleep)() = nullptr;
static void (*on_drive)() = nullptr;
static CompletionInbox *inbox = nullptr;  // PSRAM
static lv_timer_t *banner_timer = nullptr;
static void open_panel(QuickPanel &p);

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

static void close(QuickPanel &p) {
  p.open = false;
  if (p.overlay) lv_obj_add_flag(p.overlay, LV_OBJ_FLAG_HIDDEN);
  if (p.notice_text) lv_label_set_text(p.notice_text, "새 알림이 없습니다");
}

static void close_clicked(lv_event_t *e) { close(*(QuickPanel *)lv_event_get_user_data(e)); }

static void sleep_clicked(lv_event_t *) {
  for (QuickPanel &p : panels) close(p);
  if (on_sleep) on_sleep();
}

static void drive_clicked(lv_event_t *) {
  for (QuickPanel &p : panels) close(p);
  if (on_drive) on_drive();
}

static void layout(QuickPanel &p) {
  int w = lv_disp_get_hor_res(p.display), h = lv_disp_get_ver_res(p.display);
  if (p.banner) {
    lv_obj_set_size(p.banner, w - 24, LV_SIZE_CONTENT);
    lv_obj_align(p.banner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_width(p.banner_text, w - 52);
  }
  if (!p.overlay) return;
  lv_obj_set_size(p.overlay, w, h);
  lv_obj_set_pos(p.overlay, 0, 0);
  // 왼쪽은 알림, 오른쪽은 버튼 2개. 세로 화면은 폭이 좁아 버튼 이름을 줄이고 설명을 두 줄로 나눈다
  bool narrow = w < 300;
  int button_w = narrow ? 110 : 150, button_h = narrow ? 64 : 56;
  int column_h = button_h * 2 + 8, button_x = 12 + (w - 40) - button_w;
  lv_obj_set_size(p.sheet, w - 16, LV_MIN(h - 8, 56 + column_h + 52));
  lv_obj_set_pos(p.sheet, 8, 4);
  lv_obj_set_size(p.handle, 30, 3);
  lv_obj_set_pos(p.handle, (w - 16 - 30) / 2, 7);
  lv_obj_set_pos(p.title, 12, 23);
  lv_obj_set_size(p.close, 32, 30);
  lv_obj_set_pos(p.close, w - 16 - 40, 17);
  lv_obj_set_size(p.notices, w - 40 - button_w - 8, column_h);
  lv_obj_set_pos(p.notices, 12, 56);
  lv_obj_set_width(p.notice_text, lv_pct(100));
  lv_obj_set_size(p.sleep, button_w, button_h);
  lv_obj_set_pos(p.sleep, button_x, 56);
  lv_obj_set_size(p.drive, button_w, button_h);
  lv_obj_set_pos(p.drive, button_x, 56 + button_h + 8);
  for (lv_obj_t *name : {p.sleep_name, p.drive_name}) {
    lv_obj_set_style_text_font(name, narrow ? &font_ko_14 : &font_ko_20, 0);
    lv_obj_set_pos(name, 8, narrow ? 5 : 6);
  }
  for (lv_obj_t *detail : {p.sleep_detail, p.drive_detail}) {
    lv_obj_set_width(detail, button_w - 16);
    lv_obj_set_pos(detail, 8, narrow ? 24 : 32);
  }
  lv_obj_set_pos(p.hint, 12, 56 + column_h + 8);
  lv_obj_set_width(p.hint, w - 40);
}

static void build(QuickPanel &p) {
  p.overlay = lv_obj_create(lv_disp_get_layer_top(p.display));
  lv_obj_remove_style_all(p.overlay);
  lv_obj_set_style_bg_color(p.overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(p.overlay, LV_OPA_70, 0);
  lv_obj_add_flag(p.overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(p.overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(p.overlay, close_clicked, LV_EVENT_CLICKED, &p);

  p.sheet = lv_obj_create(p.overlay);
  lv_obj_remove_style_all(p.sheet);
  lv_obj_add_flag(p.sheet, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(p.sheet, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_scroll_dir(p.sheet, LV_DIR_VER);
  lv_obj_set_style_bg_color(p.sheet, lv_color_hex(0x171E26), 0);
  lv_obj_set_style_bg_opa(p.sheet, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(p.sheet, 12, 0);
  p.handle = lv_obj_create(p.sheet);
  lv_obj_remove_style_all(p.handle);
  lv_obj_clear_flag(p.handle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(p.handle, lv_color_hex(0x58616C), 0);
  lv_obj_set_style_bg_opa(p.handle, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(p.handle, 2, 0);
  p.title = label(p.sheet, "알림 / 빠른 설정", &font_ko_20, 0xE9EDF1);
  p.close = lv_btn_create(p.sheet);
  lv_obj_remove_style_all(p.close);
  lv_obj_clear_flag(p.close, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *x = label(p.close, "X", &font_ko_14, 0x8A939E);
  lv_obj_center(x);
  lv_obj_add_event_cb(p.close, close_clicked, LV_EVENT_CLICKED, &p);

  p.notices = lv_obj_create(p.sheet);
  lv_obj_remove_style_all(p.notices);
  lv_obj_set_scroll_dir(p.notices, LV_DIR_VER);
  lv_obj_clear_flag(p.notices, LV_OBJ_FLAG_GESTURE_BUBBLE);
  p.notice_text = label(p.notices, "새 알림이 없습니다", &font_ko_14, 0xB8E0CD);
  lv_label_set_long_mode(p.notice_text, LV_LABEL_LONG_WRAP);

  p.sleep = lv_btn_create(p.sheet);
  lv_obj_remove_style_all(p.sleep);
  lv_obj_clear_flag(p.sleep, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(p.sleep, lv_color_hex(0x253C38), 0);
  lv_obj_set_style_bg_opa(p.sleep, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(p.sleep, lv_color_hex(0x34564E), LV_STATE_PRESSED);
  lv_obj_set_style_radius(p.sleep, 8, 0);
  p.sleep_name = label(p.sleep, "절전 모드", &font_ko_20, 0xB8E0CD);
  p.sleep_detail = label(p.sleep, "화면 끄기 / 연결 유지", &font_ko_14, 0xB3C4BE);
  lv_label_set_long_mode(p.sleep_detail, LV_LABEL_LONG_WRAP);
  lv_obj_add_event_cb(p.sleep, sleep_clicked, LV_EVENT_CLICKED, nullptr);

  p.drive = lv_btn_create(p.sheet);
  lv_obj_remove_style_all(p.drive);
  lv_obj_clear_flag(p.drive, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(p.drive, lv_color_hex(0x1F3045), 0);
  lv_obj_set_style_bg_opa(p.drive, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(p.drive, lv_color_hex(0x2B4560), LV_STATE_PRESSED);
  lv_obj_set_style_radius(p.drive, 8, 0);
  p.drive_name = label(p.drive, "USB 드라이브", &font_ko_20, 0xB8D4F0);
  p.drive_detail = label(p.drive, "PC 에서 SD 카드 열기", &font_ko_14, 0xA9B8C8);
  lv_label_set_long_mode(p.drive_detail, LV_LABEL_LONG_WRAP);
  lv_obj_add_event_cb(p.drive, drive_clicked, LV_EVENT_CLICKED, nullptr);
  p.hint = label(p.sheet, "터치 / BOOT 버튼으로 깨우기\n상단을 위로 쓸어 닫기", &font_ko_14, 0x8A939E);
  layout(p);
  lv_obj_add_flag(p.overlay, LV_OBJ_FLAG_HIDDEN);
}

void quickpanel_init(void (*sleep_handler)(), void (*drive_handler)()) {
  on_sleep = sleep_handler;
  on_drive = drive_handler;
  void *memory = ps_malloc(sizeof(CompletionInbox));
  if (memory) inbox = new (memory) CompletionInbox();
  panels[0].display = lv_disp_get_default();
  panels[1].display = lv_obj_get_disp(display_device_screen());
}

static void hide_banners(lv_timer_t *) {
  for (QuickPanel &p : panels) if (p.banner) lv_obj_add_flag(p.banner, LV_OBJ_FLAG_HIDDEN);
  if (banner_timer) lv_timer_pause(banner_timer);
}

unsigned quickpanel_unread_count() { return inbox ? inbox->count() : 0; }

static void refresh_counts() {
  for (QuickPanel &p : panels) if (p.status_anchor) quickpanel_status_before(p.status_anchor);
  ui_layout_status();
}

static void open_panel(QuickPanel &p) {
  if (!p.overlay) build(p);
  char *snapshot = (char *)ps_malloc(CompletionInbox::SNAPSHOT_SIZE);
  if (snapshot) {
    if (inbox) inbox->take(snapshot, CompletionInbox::SNAPSHOT_SIZE);
    else snapshot[0] = 0;
    lv_label_set_text(p.notice_text, snapshot[0] ? snapshot : "새 알림이 없습니다");
    free(snapshot);
    hide_banners(nullptr);
    refresh_counts();
  }
  layout(p);
  lv_obj_scroll_to_y(p.sheet, 0, LV_ANIM_OFF);
  lv_obj_scroll_to_y(p.notices, 0, LV_ANIM_OFF);
  p.open = true;
  lv_obj_move_foreground(p.overlay);
  lv_obj_clear_flag(p.overlay, LV_OBJ_FLAG_HIDDEN);
}

static void banner_clicked(lv_event_t *e) {
  // 팝업을 닫는 것만으로는 읽음 처리하지 않는다. 상단 목록에서 다시 확인할 수 있다.
  QuickPanel &p = *(QuickPanel *)lv_event_get_user_data(e);
  lv_obj_add_flag(p.banner, LV_OBJ_FLAG_HIDDEN);
}

static void status_clicked(lv_event_t *e) { open_panel(*(QuickPanel *)lv_event_get_user_data(e)); }

lv_obj_t *quickpanel_status_before(lv_obj_t *right) {
  lv_disp_t *display = lv_obj_get_disp(right);
  QuickPanel *p = nullptr;
  for (QuickPanel &candidate : panels) if (candidate.display == display) p = &candidate;
  if (!p) for (QuickPanel &candidate : panels) if (!candidate.display) {
    candidate.display = display; p = &candidate; break;
  }
  if (!p) return right;
  p->status_anchor = right;
  if (!p->status) {
    p->status = lv_btn_create(lv_disp_get_layer_sys(display));
    lv_obj_remove_style_all(p->status);
    lv_obj_set_size(p->status, 36, 24);
    lv_obj_clear_flag(p->status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(p->status, lv_color_hex(0x2A3038), 0);
    lv_obj_set_style_bg_opa(p->status, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p->status, 5, 0);
    lv_obj_add_event_cb(p->status, status_clicked, LV_EVENT_CLICKED, p);
    p->status_text = label(p->status, "", &lv_font_montserrat_14, 0xE0A030);
    lv_obj_center(p->status_text);
  }
  unsigned count = quickpanel_unread_count();
  if (!count) {
    lv_obj_add_flag(p->status, LV_OBJ_FLAG_HIDDEN);
    return right;
  }
  lv_label_set_text_fmt(p->status_text, LV_SYMBOL_BELL " %u", count);
  lv_obj_align_to(p->status, right, LV_ALIGN_OUT_LEFT_MID, -4, 0);
  lv_obj_clear_flag(p->status, LV_OBJ_FLAG_HIDDEN);
  return p->status;
}

void quickpanel_notify(const char *text) {
  if (!inbox || !text || !*text) return;
  inbox->push(text);
  refresh_counts();
  display_poke();
  // 이미 알림을 보는 중이면 새 완료 문구도 그 자리에서 읽음 처리한다.
  for (QuickPanel &p : panels) if (p.open) { open_panel(p); return; }
  for (QuickPanel &p : panels) {
    if (!p.banner) {
      p.banner = lv_btn_create(lv_disp_get_layer_sys(p.display));
      lv_obj_remove_style_all(p.banner);
      lv_obj_clear_flag(p.banner, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_style_bg_color(p.banner, lv_color_hex(0x2A3038), 0);
      lv_obj_set_style_bg_opa(p.banner, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(p.banner, 8, 0);
      lv_obj_set_style_border_width(p.banner, 2, 0);
      lv_obj_set_style_border_color(p.banner, lv_color_hex(0xD97757), 0);
      lv_obj_set_style_pad_all(p.banner, 12, 0);
      lv_obj_set_flex_flow(p.banner, LV_FLEX_FLOW_COLUMN);
      lv_obj_add_event_cb(p.banner, banner_clicked, LV_EVENT_CLICKED, &p);
      p.banner_text = label(p.banner, "", &font_ko_20, 0xE9EDF1);
      lv_label_set_long_mode(p.banner_text, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_align(p.banner_text, LV_TEXT_ALIGN_CENTER, 0);
    }
    int w = lv_disp_get_hor_res(p.display);
    lv_obj_set_size(p.banner, w - 24, LV_SIZE_CONTENT);
    lv_obj_set_width(p.banner_text, w - 52);
    lv_label_set_text(p.banner_text, inbox->latest());
    lv_obj_update_layout(p.banner);
    lv_obj_align(p.banner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(p.banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(p.banner);
  }
  if (!banner_timer) banner_timer = lv_timer_create(hide_banners, 5000, nullptr);
  lv_timer_reset(banner_timer);
  lv_timer_resume(banner_timer);
}

bool quickpanel_is_open() {
  for (QuickPanel &p : panels) if (p.open) return true;
  return false;
}

void quickpanel_layout() {
  for (QuickPanel &p : panels) {
    p.swipe.reset();
    layout(p);
  }
}

bool quickpanel_touch(lv_disp_t *display, int x, int y, bool down) {
  if (!on_sleep || usb_drive_active()) return false;  // USB 드라이브 화면에서는 열지 않는다
  QuickPanel *p = nullptr;
  for (QuickPanel &candidate : panels) if (candidate.display == display) p = &candidate;
  if (!p) {
    for (QuickPanel &candidate : panels) if (!candidate.display) {
      candidate.display = display;
      p = &candidate;
      break;
    }
  }
  if (!p) return false;
  if (down && !p->tracking) {
    p->tracking = true;
    p->header_touch = y < 56;
  }
  if (!down) p->tracking = false;
  // 알림 목록과 아래쪽 설정은 스크롤한다. 닫기 쓸기는 판의 상단에서만 시작한다.
  if (p->open && !p->header_touch) {
    p->swipe.reset();
    return false;
  }
  QuickSwipeAction action = p->swipe.sample(x, y, down, p->open);
  if (action == QuickSwipeAction::Open) {
    open_panel(*p);
  } else if (action == QuickSwipeAction::Close) {
    close(*p);
  }
  if (action != QuickSwipeAction::None) {
    // 상단의 회전/뒤로 버튼에서 시작했어도 손을 뗄 때 원래 버튼을 누르지 않는다.
    lv_indev_t *input = lv_indev_get_act();
    if (input) lv_indev_wait_release(input);
  }
  return p->swipe.consumed();
}
