#include "quickpanel.h"
#include "quick_swipe.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

struct QuickPanel {
  lv_disp_t *display = nullptr;
  lv_obj_t *overlay = nullptr, *sheet = nullptr, *handle = nullptr;
  lv_obj_t *title = nullptr, *close = nullptr, *sleep = nullptr, *hint = nullptr;
  bool open = false;
  QuickSwipe swipe;
};
static QuickPanel panels[2];  // 원래 화면과 미러링 중 기기 화면은 서로 다른 LVGL 디스플레이
static void (*on_sleep)() = nullptr;

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
}

static void close_clicked(lv_event_t *e) { close(*(QuickPanel *)lv_event_get_user_data(e)); }

static void sleep_clicked(lv_event_t *) {
  for (QuickPanel &p : panels) close(p);
  if (on_sleep) on_sleep();
}

static void layout(QuickPanel &p) {
  if (!p.overlay) return;
  int w = lv_disp_get_hor_res(p.display), h = lv_disp_get_ver_res(p.display);
  lv_obj_set_size(p.overlay, w, h);
  lv_obj_set_pos(p.overlay, 0, 0);
  lv_obj_set_size(p.sheet, w - 16, 184);
  lv_obj_set_pos(p.sheet, 8, 4);
  lv_obj_set_size(p.handle, 30, 3);
  lv_obj_set_pos(p.handle, (w - 16 - 30) / 2, 7);
  lv_obj_set_pos(p.title, 12, 23);
  lv_obj_set_size(p.close, 32, 30);
  lv_obj_set_pos(p.close, w - 16 - 40, 17);
  lv_obj_set_size(p.sleep, w - 40, 68);
  lv_obj_set_pos(p.sleep, 12, 56);
  lv_obj_set_pos(p.hint, 12, 136);
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
  lv_obj_clear_flag(p.sheet, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_style_bg_color(p.sheet, lv_color_hex(0x171E26), 0);
  lv_obj_set_style_bg_opa(p.sheet, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(p.sheet, 12, 0);
  p.handle = lv_obj_create(p.sheet);
  lv_obj_remove_style_all(p.handle);
  lv_obj_clear_flag(p.handle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(p.handle, lv_color_hex(0x58616C), 0);
  lv_obj_set_style_bg_opa(p.handle, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(p.handle, 2, 0);
  p.title = label(p.sheet, "빠른 설정", &font_ko_20, 0xE9EDF1);
  p.close = lv_btn_create(p.sheet);
  lv_obj_remove_style_all(p.close);
  lv_obj_clear_flag(p.close, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *x = label(p.close, "X", &font_ko_14, 0x8A939E);
  lv_obj_center(x);
  lv_obj_add_event_cb(p.close, close_clicked, LV_EVENT_CLICKED, &p);

  p.sleep = lv_btn_create(p.sheet);
  lv_obj_remove_style_all(p.sleep);
  lv_obj_clear_flag(p.sleep, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(p.sleep, lv_color_hex(0x253C38), 0);
  lv_obj_set_style_bg_opa(p.sleep, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(p.sleep, lv_color_hex(0x34564E), LV_STATE_PRESSED);
  lv_obj_set_style_radius(p.sleep, 8, 0);
  lv_obj_t *name = label(p.sleep, "절전 모드", &font_ko_20, 0xB8E0CD);
  lv_obj_set_pos(name, 12, 10);
  lv_obj_t *detail = label(p.sleep, "화면 끄기 / 연결 유지", &font_ko_14, 0xB3C4BE);
  lv_obj_set_pos(detail, 12, 40);
  lv_obj_add_event_cb(p.sleep, sleep_clicked, LV_EVENT_CLICKED, nullptr);
  p.hint = label(p.sheet, "터치 / BOOT 버튼으로 깨우기\n위로 쓸어 닫기", &font_ko_14, 0x8A939E);
  layout(p);
  lv_obj_add_flag(p.overlay, LV_OBJ_FLAG_HIDDEN);
}

void quickpanel_init(void (*sleep_handler)()) { on_sleep = sleep_handler; }

void quickpanel_layout() {
  for (QuickPanel &p : panels) {
    p.swipe.reset();
    layout(p);
  }
}

bool quickpanel_touch(lv_disp_t *display, int x, int y, bool down) {
  if (!on_sleep) return false;
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
  QuickSwipeAction action = p->swipe.sample(x, y, down, p->open);
  if (action == QuickSwipeAction::Open) {
    if (!p->overlay) build(*p);
    layout(*p);
    p->open = true;
    lv_obj_move_foreground(p->overlay);
    lv_obj_clear_flag(p->overlay, LV_OBJ_FLAG_HIDDEN);
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
