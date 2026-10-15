#include "camera_ui.h"

#include <lvgl.h>

#include "camera.h"
#include "display.h"
#include "photo.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define PREVIEW_MAX_PIXELS (320 * 240)
#define SHUTTER_SIZE 60

static lv_obj_t *overlay;
static lv_obj_t *preview;
static lv_obj_t *shutter;
static lv_obj_t *close_btn;
static lv_obj_t *msg_lbl;
static lv_img_dsc_t frame_dsc;
static uint16_t *frame_buf;
static bool is_open = false;
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
  if (camera_start()) {
    lv_obj_add_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(shutter, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(shutter, LV_OBJ_FLAG_HIDDEN);
    show_message("카메라가 연결되어 있지 않습니다", 0);
  }
}

static void take_photo() {
  if (!camera_running()) return;
  show_message("찍는 중...", 0);
  lv_refr_now(NULL);
  uint8_t *jpg;
  size_t len;
  if (camera_capture(display_rotation(), &jpg, &len)) {
    show_message(photo_save(jpg, len), 2500);
  } else {
    show_message("찍지 못했습니다", 2500);
  }
}

static void screen_gesture(lv_event_t *e) {
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_TOP) open_camera();
}

static void overlay_gesture(lv_event_t *e) {
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_BOTTOM) close_camera();
}

static void shutter_clicked(lv_event_t *e) {
  take_photo();
}

static void close_clicked(lv_event_t *e) {
  close_camera();
}

void camera_ui_init() {
  frame_buf = (uint16_t *)ps_malloc(PREVIEW_MAX_PIXELS * 2);
  frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  frame_dsc.data = (const uint8_t *)frame_buf;

  // 사용량 화면에서 위로 밀면 열린다
  lv_obj_add_event_cb(lv_scr_act(), screen_gesture, LV_EVENT_GESTURE, NULL);

  // 사용량 화면 위에 덮는 검은 화면. 아래로 밀면 닫힌다.
  overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(overlay);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(overlay, overlay_gesture, LV_EVENT_GESTURE, NULL);

  preview = lv_img_create(overlay);

  // 촬영 버튼: 흰 테두리의 동그란 버튼
  shutter = lv_btn_create(overlay);
  lv_obj_set_size(shutter, SHUTTER_SIZE, SHUTTER_SIZE);
  lv_obj_set_style_radius(shutter, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(shutter, lv_color_white(), 0);
  lv_obj_set_style_bg_color(shutter, lv_color_hex(0xB0B0B0), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(shutter, lv_color_hex(0x606060), 0);
  lv_obj_set_style_border_width(shutter, 4, 0);
  lv_obj_set_style_shadow_width(shutter, 0, 0);
  lv_obj_set_ext_click_area(shutter, 8);
  lv_obj_add_event_cb(shutter, shutter_clicked, LV_EVENT_CLICKED, NULL);

  // 닫기 버튼 (왼쪽 위)
  close_btn = lv_btn_create(overlay);
  lv_obj_set_size(close_btn, 36, 36);
  lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(close_btn, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(close_btn, LV_OPA_60, 0);
  lv_obj_set_style_shadow_width(close_btn, 0, 0);
  lv_obj_set_ext_click_area(close_btn, 8);
  lv_obj_add_event_cb(close_btn, close_clicked, LV_EVENT_CLICKED, NULL);
  lv_obj_t *x = lv_label_create(close_btn);
  lv_label_set_text(x, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_color(x, lv_color_white(), 0);
  lv_obj_center(x);

  msg_lbl = lv_label_create(overlay);
  lv_obj_set_style_text_font(msg_lbl, &font_ko_20, 0);
  lv_obj_set_style_text_color(msg_lbl, lv_color_white(), 0);
  lv_obj_set_style_text_align(msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(msg_lbl, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(msg_lbl, LV_OPA_70, 0);
  lv_obj_set_style_pad_all(msg_lbl, 8, 0);

  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

void camera_ui_layout() {
  int w = display_width(), h = display_height();
  lv_obj_set_size(overlay, w, h);
  lv_obj_set_pos(overlay, 0, 0);
  // 가로 화면이면 오른쪽 가운데, 세로 화면이면 아래 가운데
  if (w > h) {
    lv_obj_align(shutter, LV_ALIGN_RIGHT_MID, -10, 0);
  } else {
    lv_obj_align(shutter, LV_ALIGN_BOTTOM_MID, 0, -10);
  }
  lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 6, 6);
  lv_obj_center(msg_lbl);
  lv_obj_center(preview);
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
  if (!camera_running() || millis() - frame_ms < 40) return;
  frame_ms = millis();
  int w, h;
  if (!camera_preview(frame_buf, display_width(), display_height(), display_rotation(), &w, &h)) return;
  static uint32_t frames = 0, counted_ms = 0;
  frames++;
  if (millis() - counted_ms >= 5000) {
    if (counted_ms) Serial.printf("[camera] preview %dx%d, %.1f fps\n", w, h, frames * 1000.0f / (millis() - counted_ms));
    counted_ms = millis();
    frames = 0;
  }
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

void camera_ui_command(const char *cmd) {
  if (strcmp(cmd, "open") == 0) {
    open_camera();
  } else if (strcmp(cmd, "shoot") == 0) {
    open_camera();
    take_photo();
  } else if (strcmp(cmd, "close") == 0) {
    close_camera();
  }
}
