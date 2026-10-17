#include "camera_ui.h"

#include <img_converters.h>
#include <lvgl.h>

#include "camera.h"
#include "display.h"
#include "photo.h"
#include "ui.h"
#include "link.h"
#include "video.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define PREVIEW_MAX_PIXELS (320 * 240)
#define SHUTTER_SIZE 60

static lv_obj_t *overlay;
static lv_obj_t *preview;
static lv_obj_t *shutter;
static lv_obj_t *close_btn;
static lv_obj_t *record_btn;
static lv_obj_t *record_dot;
static lv_obj_t *rec_lbl;
static lv_obj_t *msg_lbl;
static lv_obj_t *send_lbl;
static lv_img_dsc_t frame_dsc;
static uint16_t *frame_buf;
static bool is_open = false;
static uint32_t msg_until = 0;

static void show_message(const char *text, uint32_t ms) {
  lv_label_set_text(msg_lbl, text);
  lv_obj_clear_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
  msg_until = ms ? millis() + ms : 0;
}

static void stop_recording() {
  if (!video_recording()) return;
  const char *result = video_stop();
  camera_set_video(false, nullptr);
  // 녹화 중에는 가운데 점을 네모로 바꿔 "멈춤" 버튼처럼 보이게 했다
  lv_obj_set_style_radius(record_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_size(record_dot, 22, 22);
  lv_obj_add_flag(rec_lbl, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(shutter, LV_OBJ_FLAG_HIDDEN);
  if (result) show_message(result, 2500);
}

static void start_recording() {
  if (!camera_running() || video_recording()) return;
  const char *error = video_start(camera_turns(display_rotation()));
  if (error) {
    show_message(error, 3000);
    return;
  }
  camera_set_video(true, video_frame);
  lv_obj_set_style_radius(record_dot, 3, 0);
  lv_obj_set_size(record_dot, 18, 18);
  lv_obj_clear_flag(rec_lbl, LV_OBJ_FLAG_HIDDEN);
  // 녹화 중에는 사진 찍기를 숨긴다
  lv_obj_add_flag(shutter, LV_OBJ_FLAG_HIDDEN);
}

static void close_camera() {
  if (!is_open) return;
  stop_recording();
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
    lv_obj_clear_flag(record_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(shutter, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(record_btn, LV_OBJ_FLAG_HIDDEN);
    show_message(camera_missing() ? "카메라가 연결되어 있지 않습니다" : "카메라를 켜지 못했습니다\n기기를 재시작해 보세요", 0);
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

static void overlay_gesture(lv_event_t *e) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  // 오른쪽으로 밀면 홈으로, 아래로 밀면 닫고 보던 화면으로
  if (dir == LV_DIR_RIGHT) {
    close_camera();
    ui_show_home();
  } else if (dir == LV_DIR_BOTTOM) {
    close_camera();
  }
}

static void shutter_clicked(lv_event_t *e) {
  take_photo();
}

static void record_clicked(lv_event_t *e) {
  if (video_recording()) {
    stop_recording();
  } else {
    start_recording();
  }
}

static void close_clicked(lv_event_t *e) {
  close_camera();
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 36, 36);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_ext_click_area(btn, 8);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, symbol);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_center(lbl);
  return btn;
}

void camera_ui_init() {
  frame_buf = (uint16_t *)ps_malloc(PREVIEW_MAX_PIXELS * 2);
  frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  frame_dsc.data = (const uint8_t *)frame_buf;

  // 화면 위에 덮는 검은 화면. 오른쪽으로 밀면 홈으로, 아래로 밀면 닫힌다.
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

  // 녹화 버튼: 촬영 버튼 옆의 작은 버튼 (가운데 빨간 점, 녹화 중에는 빨간 네모)
  record_btn = lv_btn_create(overlay);
  lv_obj_set_size(record_btn, 44, 44);
  lv_obj_set_style_radius(record_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(record_btn, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(record_btn, LV_OPA_60, 0);
  lv_obj_set_style_border_color(record_btn, lv_color_white(), 0);
  lv_obj_set_style_border_width(record_btn, 3, 0);
  lv_obj_set_style_shadow_width(record_btn, 0, 0);
  lv_obj_set_ext_click_area(record_btn, 8);
  lv_obj_add_event_cb(record_btn, record_clicked, LV_EVENT_CLICKED, NULL);
  record_dot = lv_obj_create(record_btn);
  lv_obj_remove_style_all(record_dot);
  lv_obj_set_size(record_dot, 22, 22);
  lv_obj_set_style_radius(record_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(record_dot, lv_color_hex(0xE0302A), 0);
  lv_obj_set_style_bg_opa(record_dot, LV_OPA_COVER, 0);
  lv_obj_clear_flag(record_dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(record_dot);

  // 녹화 시간 (위 가운데)
  rec_lbl = lv_label_create(overlay);
  lv_obj_set_style_text_font(rec_lbl, &font_ko_20, 0);
  lv_obj_set_style_text_color(rec_lbl, lv_color_white(), 0);
  lv_obj_set_style_bg_color(rec_lbl, lv_color_hex(0xC0201A), 0);
  lv_obj_set_style_bg_opa(rec_lbl, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(rec_lbl, 4, 0);
  lv_obj_set_style_pad_hor(rec_lbl, 6, 0);
  lv_label_set_text(rec_lbl, "0:00");
  lv_obj_add_flag(rec_lbl, LV_OBJ_FLAG_HIDDEN);

  // 왼쪽 위: 닫기. (카메라 방향은 촬영 중 실수로 바뀌지 않게 설정 화면에서만 바꾼다)
  close_btn = round_button(overlay, LV_SYMBOL_CLOSE, close_clicked);

  msg_lbl = lv_label_create(overlay);
  lv_obj_set_style_text_font(msg_lbl, &font_ko_20, 0);
  lv_obj_set_style_text_color(msg_lbl, lv_color_white(), 0);
  lv_obj_set_style_text_align(msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(msg_lbl, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(msg_lbl, LV_OPA_70, 0);
  lv_obj_set_style_pad_all(msg_lbl, 8, 0);

  // 위쪽 가운데: PC 로 보내는 진행률
  send_lbl = lv_label_create(overlay);
  lv_obj_set_style_text_font(send_lbl, &font_ko_14, 0);
  lv_obj_set_style_text_color(send_lbl, lv_color_white(), 0);
  lv_obj_set_style_bg_color(send_lbl, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(send_lbl, LV_OPA_60, 0);
  lv_obj_set_style_radius(send_lbl, 4, 0);
  lv_obj_set_style_pad_hor(send_lbl, 6, 0);
  lv_label_set_text(send_lbl, "");
  lv_obj_add_flag(send_lbl, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

void camera_ui_layout() {
  int w = display_width(), h = display_height();
  lv_obj_set_size(overlay, w, h);
  lv_obj_set_pos(overlay, 0, 0);
  // 가로 화면이면 오른쪽 가운데, 세로 화면이면 아래 가운데
  if (w > h) {
    lv_obj_align(shutter, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_align(record_btn, LV_ALIGN_RIGHT_MID, -18, 70);
  } else {
    lv_obj_align(shutter, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_align(record_btn, LV_ALIGN_BOTTOM_MID, 70, -18);
  }
  lv_obj_align(rec_lbl, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_align(send_lbl, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 6, 6);
  lv_obj_center(msg_lbl);
  lv_obj_center(preview);
}

bool camera_ui_open() {
  return is_open;
}

// 사진을 PC 로 보내는 동안 진행률을 보이고, 오래 걸린 전송(블루투스)이 끝나면 알린다.
// 카메라를 닫은 뒤에 끝나도 알림은 뜬다.
static void show_send_progress() {
  static uint32_t seen = 0;
  bool ok;
  uint32_t ms;
  uint32_t count = photo_finished(&ok, &ms);
  if (count != seen) {
    seen = count;
    if (ms > 2000) ui_toast(ok ? "사진을 PC에 보냈습니다" : "사진을 PC에 보내지 못했습니다", 2500);
  }
  int pct;
  if (is_open && photo_progress(&pct)) {
    char t[40];
    snprintf(t, sizeof(t), "PC로 보내는 중 %d%%", pct);
    if (strcmp(lv_label_get_text(send_lbl), t) != 0) lv_label_set_text(send_lbl, t);
    lv_obj_clear_flag(send_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(send_lbl, LV_OBJ_FLAG_HIDDEN);
  }
}

void camera_ui_loop() {
  show_send_progress();
  if (!is_open) return;
  if (msg_until && (int32_t)(millis() - msg_until) > 0) {
    msg_until = 0;
    lv_obj_add_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
  }
  static uint32_t frame_ms = 0;
  if (!camera_running() || (!video_recording() && millis() - frame_ms < 40)) return;
  frame_ms = millis();
  int w, h;
  if (video_recording()) {
    char t[16];
    uint32_t s = video_elapsed_ms() / 1000;
    snprintf(t, sizeof(t), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    if (strcmp(lv_label_get_text(rec_lbl), t) != 0) lv_label_set_text(rec_lbl, t);
    // 녹화 중 PC 연결이 끊기면 멈춘다
    if (link_active() != LINK_USB && link_active() != LINK_WIFI) stop_recording();
  }
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
  } else if (strcmp(cmd, "dump") == 0 && frame_dsc.header.w) {
    // 진단용: 화면에 그리고 있는 미리보기를 그대로 사진으로 보낸다 (PC: --camera dump)
    int w = frame_dsc.header.w, h = frame_dsc.header.h;
    uint8_t *be = (uint8_t *)ps_malloc(w * h * 2);
    for (int i = 0; i < w * h; i++) {
      be[2 * i] = frame_buf[i] >> 8;
      be[2 * i + 1] = frame_buf[i] & 0xFF;
    }
    uint8_t *jpg;
    size_t len;
    if (fmt2jpg(be, w * h * 2, w, h, PIXFORMAT_RGB565, 90, &jpg, &len)) photo_save(jpg, len, false);
    free(be);
  } else if (strcmp(cmd, "record") == 0) {
    open_camera();
    if (video_recording()) {
      stop_recording();
    } else {
      start_recording();
    }
  } else if (strcmp(cmd, "close") == 0) {
    close_camera();
  }
}

void camera_ui_show() {
  open_camera();
}
