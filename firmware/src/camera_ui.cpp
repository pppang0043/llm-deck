#include "camera_ui.h"

#include <lvgl.h>

#include "camera.h"
#include "display.h"
#include "photo.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define PREVIEW_MAX_PIXELS (320 * 240)

static lv_obj_t *overlay;
static lv_obj_t *preview;
static lv_obj_t *hint_lbl;
static lv_obj_t *msg_lbl;
static lv_img_dsc_t frame_dsc;
static uint16_t *frame_buf;
static bool is_open = false;
static bool swiped = false;  // 밀기 동작 뒤에 따라오는 "눌림"은 촬영으로 치지 않는다
static uint32_t msg_until = 0;

static void show_message(const char *text, uint32_t ms) {
  lv_label_set_text(msg_lbl, text);
  lv_obj_clear_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
  msg_until = ms ? millis() + ms : 0;
}

static void close_camera() {
  if (!is_open) return;
  is_open = false;
  camera_stop();
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

static void open_camera() {
  if (is_open) return;
  is_open = true;
  camera_ui_layout();
  lv_obj_add_flag(preview, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  show_message("카메라 켜는 중...", 0);
  lv_refr_now(NULL);
  if (camera_start(display_width() > display_height())) {
    lv_obj_add_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    show_message("카메라가 연결되어 있지 않습니다\n\n아래로 밀면 닫힙니다", 0);
  }
}

static void take_photo() {
  if (!camera_running()) return;
  show_message("찍는 중...", 0);
  lv_refr_now(NULL);
  uint8_t *jpg;
  size_t len;
  if (camera_capture(&jpg, &len)) {
    show_message(photo_save(jpg, len), 2500);
  } else {
    show_message("찍지 못했습니다", 2500);
  }
}

static void screen_gesture(lv_event_t *e) {
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_TOP) open_camera();
}

static void overlay_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    swiped = false;
  } else if (code == LV_EVENT_GESTURE) {
    swiped = true;
    if (lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_BOTTOM) close_camera();
  } else if (code == LV_EVENT_CLICKED && !swiped) {
    take_photo();
  }
}

void camera_ui_init() {
  frame_buf = (uint16_t *)ps_malloc(PREVIEW_MAX_PIXELS * 2);
  frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  frame_dsc.data = (const uint8_t *)frame_buf;

  // 위로 밀면 열린다
  lv_obj_add_event_cb(lv_scr_act(), screen_gesture, LV_EVENT_GESTURE, NULL);

  // 사용량 화면 위에 덮는 검은 화면
  overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(overlay);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(overlay, overlay_event, LV_EVENT_ALL, NULL);

  preview = lv_img_create(overlay);
  lv_obj_center(preview);

  hint_lbl = lv_label_create(overlay);
  lv_label_set_text(hint_lbl, "누르면 촬영 | 아래로 밀면 닫기");
  lv_obj_set_style_text_font(hint_lbl, &font_ko_14, 0);
  lv_obj_set_style_text_color(hint_lbl, lv_color_hex(0xC0C6CC), 0);
  lv_obj_set_style_bg_color(hint_lbl, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(hint_lbl, LV_OPA_50, 0);
  lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);

  msg_lbl = lv_label_create(overlay);
  lv_obj_set_style_text_font(msg_lbl, &font_ko_20, 0);
  lv_obj_set_style_text_color(msg_lbl, lv_color_white(), 0);
  lv_obj_set_style_text_align(msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(msg_lbl, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(msg_lbl, LV_OPA_70, 0);
  lv_obj_set_style_pad_all(msg_lbl, 8, 0);
  lv_obj_center(msg_lbl);

  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

void camera_ui_layout() {
  lv_obj_set_size(overlay, display_width(), display_height());
  lv_obj_set_pos(overlay, 0, 0);
  if (is_open) camera_set_landscape(display_width() > display_height());
}

bool camera_ui_open() {
  return is_open;
}

void camera_ui_loop() {
  if (!is_open) return;
  if (msg_until && (int32_t)(millis() - msg_until) > 0) {
    msg_until = 0;
    lv_obj_add_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
  }
  static uint32_t frame_ms = 0;
  if (!camera_running() || millis() - frame_ms < 50) return;
  frame_ms = millis();
  int w, h;
  if (!camera_preview(frame_buf, PREVIEW_MAX_PIXELS, &w, &h)) return;
  if (frame_dsc.header.w != w || frame_dsc.header.h != h) {
    frame_dsc.header.w = w;
    frame_dsc.header.h = h;
    frame_dsc.data_size = w * h * 2;
    lv_img_set_src(preview, &frame_dsc);
    lv_obj_center(preview);
  }
  lv_obj_clear_flag(preview, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(preview);
}
