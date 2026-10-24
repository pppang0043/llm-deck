#include "camera_ui.h"

#include <img_converters.h>
#include <freertos/idf_additions.h>
#include <lvgl.h>

#include "camera.h"
#include "display.h"
#include "gallery.h"
#include "imu.h"
#include "photo.h"
#include "sd_video.h"
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
static lv_obj_t *gallery_btn;
static lv_obj_t *adjust_btn;
// 가운데 안내 네모: QR·바코드를 대략 이 안에 두면 된다 (자리는 스캔이 찾는다, companion/scanner.py)
static lv_obj_t *guide, *guide_lbl;
// 스캔 (2단계): 넓은 장(LOC_)을 보내면 PC 가 바코드 자리를 돌려주고({"scan_roi"}), 그 자리만 원본 화소로 찍은
// 장(SCAN_)을 보낸다. PC 가 읽으면({"scan"}) 멈춘다. 갤러리·SD 에는 남기지 않는다
static lv_obj_t *scan_btn;
static bool scanning = false;
enum ScanStage { SCAN_LOCATE, SCAN_WAIT_ROI, SCAN_FOCUS, SCAN_ZOOM };
static ScanStage scan_stage = SCAN_LOCATE;
static uint32_t scan_started = 0, scan_stage_at = 0, scan_frames = 0;
static int zoom_shots = 0;
#define SCAN_LIMIT_MS 30000
#define SCAN_ROI_WAIT_MS 3000  // PC 가 자리를 이만큼 안에 안 알려 주면 다시 넓게 찍는다
#define SCAN_ZOOM_SHOTS 4      // 2단계에서 이만큼 못 읽으면 다시 자리부터 (바코드를 옮겼을 수 있다)
// 스캔으로 읽은 뒤에는 카메라를 금방 닫는다 (센서가 뜨거워진다: 2026-10-24 시험 중 발열로 중단)
#define SCAN_DONE_IDLE_MS 20000
static uint32_t scan_done_at = 0;  // 마지막으로 읽은 시각 (0 이면 아직)

// 카메라 작업(사진·스캔 장 찍기, 초점)은 따로 도는 태스크가 한다: 찍는 0.2~2.5초 동안 화면이 멈추지 않게.
// 결과는 camera_ui_loop 가 받아 저장·전송한다(job_finish). 작업 중에는 미리보기를 쉰다 (카메라를 같이 쓰지 않게)
enum CamJob { JOB_NONE, JOB_PHOTO, JOB_LOCATE, JOB_GRAB, JOB_ZOOM };
static volatile CamJob job = JOB_NONE;
static volatile bool job_done = false;
static bool job_ok = false;
static uint8_t *job_jpg = nullptr;
static size_t job_len = 0;
static uint8_t job_rot = 0;
static int job_box[4];
static TaskHandle_t job_task = nullptr;

static void job_worker(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    switch (job) {
      case JOB_PHOTO: job_ok = camera_capture(job_rot, &job_jpg, &job_len); break;
      case JOB_LOCATE: job_ok = camera_scan_locate(job_rot, &job_jpg, &job_len); break;
      case JOB_GRAB: job_ok = camera_scan_grab(job_rot, &job_jpg, &job_len); break;
      case JOB_ZOOM: job_ok = camera_scan_zoom(job_box[0], job_box[1], job_box[2], job_box[3]); break;
      default: job_ok = false; break;
    }
    job_done = true;
  }
}

// 사진 방향은 기울기 센서로 (화면을 손으로 돌려 두었어도 실제로 든 방향대로). 눕혀 두어 모르면 화면 방향
static uint8_t photo_rot() {
  int tilt = imu_rotation();
  return tilt >= 0 ? tilt : display_rotation();
}

static bool job_start(CamJob j) {
  if (job != JOB_NONE) return false;
  if (!job_task &&
      xTaskCreatePinnedToCoreWithCaps(job_worker, "camjob", 6144, NULL, 3, &job_task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
    job_task = nullptr;
    return false;
  }
  job_rot = photo_rot();
  job_ok = false;
  job_jpg = nullptr;
  job_done = false;
  job = j;
  xTaskNotifyGive(job_task);
  return true;
}

// 하던 작업이 끝날 때까지 기다리고 결과는 버린다 (카메라 크기를 바꾸거나 끄기 전에)
static void job_wait_drop() {
  uint32_t t0 = millis();
  while (job != JOB_NONE && !job_done && millis() - t0 < 8000) delay(10);
  if (job_done && job_ok && job_jpg) free(job_jpg);
  job_jpg = nullptr;
  job_done = false;
  job = JOB_NONE;
}

// 칩 온도 감시: 카메라를 켠 동안 뜨거워지면 끄고, 식을 때까지 다시 켜지 않는다.
// ESP32 칩 온도라 카메라 센서 온도와 같지는 않다. 로그([camera] chip)를 보고 기준을 맞춘다
#define CAMERA_HOT_C 75.0f
#define CAMERA_COOL_C 65.0f
static bool too_hot = false;
static uint32_t temp_at = 0, temp_log_at = 0;
static lv_obj_t *adjust_panel;  // 화질 조절 (camera_ctl_*): 미리보기를 보면서 바꾼다
static lv_obj_t **adjust_vals;
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
  lv_obj_clear_flag(gallery_btn, LV_OBJ_FLAG_HIDDEN);
  if (result) show_message(result, 2500);
}

static void start_recording() {
  if (!camera_running() || video_recording() || job != JOB_NONE) return;  // 찍는 중이면 다음에
  const char *error = video_start(camera_turns(display_rotation()));
  if (error) {
    show_message(error, 3000);
    return;
  }
  camera_set_video(true, video_frame);
  lv_obj_set_style_radius(record_dot, 3, 0);
  lv_obj_set_size(record_dot, 18, 18);
  lv_obj_clear_flag(rec_lbl, LV_OBJ_FLAG_HIDDEN);
  // 녹화 중에는 사진 찍기와 갤러리를 숨긴다
  lv_obj_add_flag(shutter, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(gallery_btn, LV_OBJ_FLAG_HIDDEN);
}

static void scan_stop(const char *msg, uint32_t ms = 3000) {
  if (scanning) {
    job_wait_drop();
    camera_scan_end();
  }
  scanning = false;
  lv_obj_set_style_border_color(guide, lv_color_white(), 0);
  if (msg) {
    show_message(msg, ms);
  } else {
    lv_obj_add_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
  }
}

static void close_camera() {
  if (!is_open) return;
  stop_recording();
  job_wait_drop();
  if (scanning) camera_scan_end();
  scanning = false;
  is_open = false;
  lv_obj_add_flag(adjust_panel, LV_OBJ_FLAG_HIDDEN);
  camera_stop();
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

static void open_camera() {
  if (is_open) return;
  if (too_hot) {
    float t = temperatureRead();
    if (t > CAMERA_COOL_C) {
      char msg[64];
      snprintf(msg, sizeof(msg), "기기가 식을 때까지 카메라를 켤 수 없습니다\n(지금 %.0f도)", t);
      ui_toast(msg, 3000);
      return;
    }
    too_hot = false;
  }
  is_open = true;
  scan_done_at = 0;
  display_poke();  // PC 에서 열었을 때도 2분 세기를 지금부터
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

static bool take_photo() {
  if (!camera_running()) return false;
  if (scanning) scan_stop(nullptr);  // 스캔 크기(가운데)에서 사진 크기로 돌아간 뒤 찍는다
  // 사진은 크기를 바꿔 찍으므로 녹화 중에 찍으면 녹화 크기(640)로 돌아오지 않아 녹화가 끊긴다
  if (video_recording()) {
    show_message("녹화 중에는 사진을 찍을 수 없습니다", 2500);
    return false;
  }
  if (!job_start(JOB_PHOTO)) return false;  // 결과는 job_finish 가 저장한다
  show_message("찍는 중...", 0);
  return true;
}

static void overlay_gesture(lv_event_t *e) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  // 오른쪽으로 밀면 홈으로, 아래로 밀면 닫고 보던 화면으로
  if (dir == LV_DIR_RIGHT) {
    close_camera();
    ui_show_home();
  } else if (dir == LV_DIR_BOTTOM) {
    close_camera();
  } else {
    return;
  }
  // 밀기를 셔터 버튼 위에서 시작했어도 손을 뗄 때 찍히지 않게 (ui.cpp screen_gesture 참고)
  lv_indev_wait_release(lv_indev_get_act());
}

static void scan_clicked(lv_event_t *e) {
  if (scanning) {
    scan_stop("스캔을 멈췄습니다");
    return;
  }
  if (!camera_running() || video_recording()) return;
  if (link_active() != LINK_USB && link_active() != LINK_BLE) {
    show_message("스캔은 PC가 USB나 블루투스로\n연결되어 있어야 합니다", 2500);
    return;
  }
  scanning = true;
  scan_started = millis();
  scan_stage = SCAN_LOCATE;
  scan_frames = 0;
  lv_obj_set_style_border_color(guide, lv_color_hex(0x3CD070), 0);
  show_message("위치 찾는 중...", 0);
}

static bool scan_send(const char *prefix, uint8_t *jpg, size_t len) {
  char name[24];
  snprintf(name, sizeof(name), "%s_%lu.jpg", prefix, (unsigned long)++scan_frames);
  if (photo_send_file(name, jpg, len)) return true;
  scan_stop("PC로 보내지 못했습니다");
  return false;
}

// 앞 장을 다 보냈고 하던 작업이 없으면 단계에 맞게 다음 장을 찍게 한다 (결과는 job_finish)
static void scan_step() {
  if (!scanning || photo_sending() || job != JOB_NONE) return;
  if (millis() - scan_started > SCAN_LIMIT_MS) {
    scan_stop("30초 동안 찾지 못했습니다\n20cm 이상 떨어져, 밝은 곳에서", 4000);
    return;
  }
  if (scan_stage == SCAN_WAIT_ROI) {
    if (millis() - scan_stage_at > SCAN_ROI_WAIT_MS) scan_stage = SCAN_LOCATE;
    return;
  }
  if (scan_stage == SCAN_ZOOM && zoom_shots >= SCAN_ZOOM_SHOTS) {
    scan_stage = SCAN_LOCATE;  // 넓게 찍으면서 2단계 창도 풀린다
    show_message("위치 찾는 중...", 0);
  }
  if (scan_stage == SCAN_LOCATE) job_start(JOB_LOCATE);
  else if (scan_stage == SCAN_ZOOM) job_start(JOB_GRAB);
}

// PC 가 1단계 장에서 찾은 바코드 자리 (0~1000). 비어 있으면 못 찾음: 다시 넓게 찍는다
void camera_ui_scan_roi(const int *box, bool found) {
  if (!scanning || scan_stage != SCAN_WAIT_ROI) return;
  if (!found) {
    scan_stage = SCAN_LOCATE;
    return;
  }
  memcpy(job_box, box, sizeof(job_box));
  if (!job_start(JOB_ZOOM)) {
    scan_stage = SCAN_LOCATE;
    return;
  }
  scan_stage = SCAN_FOCUS;
  show_message("초점 맞추는 중...", 0);
}

// 작업 태스크가 끝낸 결과를 받는다 (화면 루프에서)
static void job_finish() {
  if (job == JOB_NONE || !job_done) return;
  CamJob j = job;
  bool ok = job_ok;
  uint8_t *jpg = job_jpg;
  size_t len = job_len;
  job_jpg = nullptr;
  job_done = false;
  job = JOB_NONE;
  if (j == JOB_PHOTO) {
    show_message(ok ? photo_save(jpg, len) : "찍지 못했습니다", 2500);
    return;
  }
  if (!scanning) {  // 그 사이 스캔을 멈췄다
    if (ok && jpg) free(jpg);
    if (j == JOB_ZOOM && ok) camera_scan_end();
    return;
  }
  if (j == JOB_LOCATE) {
    if (ok && scan_send("LOC", jpg, len)) {
      scan_stage = SCAN_WAIT_ROI;
      scan_stage_at = millis();
    }
  } else if (j == JOB_GRAB) {
    if (ok && scan_send("SCAN", jpg, len)) zoom_shots++;
  } else if (j == JOB_ZOOM) {
    scan_stage = ok ? SCAN_ZOOM : SCAN_LOCATE;
    zoom_shots = 0;
    show_message(ok ? "읽는 중..." : "위치 찾는 중...", 0);
  }
}

void camera_ui_scan_result(const char *text) {
  if (!scanning) return;  // 멈춘 뒤 늦게 온 결과
  Serial.printf("[scan] found in %lu ms after %lu shots\n", (unsigned long)(millis() - scan_started),
                (unsigned long)scan_frames);
  scan_done_at = millis();
  scan_stop(text, 5000);
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

// 갤러리를 보는 동안은 카메라를 끄고 (열이 나지 않게), 닫으면 다시 켠다
static void open_gallery() {
  close_camera();
  gallery_show(open_camera);
}

// ---- 화질 조절 ----

static void show_adjust_values() {
  for (int i = 0; i < camera_ctl_count(); i++) {
    int v;
    char t[24] = "-";
    if (camera_ctl_value(i, &v)) camera_ctl_text(i, v, t, sizeof(t));
    lv_label_set_text(adjust_vals[i], t);
  }
}

static void adjust_step(lv_event_t *e) {
  if (job != JOB_NONE) return;  // 작업 태스크가 센서를 쓰는 중
  intptr_t code = (intptr_t)lv_event_get_user_data(e);  // 칸 번호 * 2 + (1 이면 +)
  if (!camera_ctl_step(code / 2, code % 2 ? 1 : -1)) show_message("바꿀 수 없습니다", 1200);
  show_adjust_values();  // 하나를 바꾸면 다른 값(수동 노출 등)도 바뀔 수 있다
}

static void adjust_toggle(lv_event_t *e) {
  if (lv_obj_has_flag(adjust_panel, LV_OBJ_FLAG_HIDDEN)) {
    show_adjust_values();
    lv_obj_clear_flag(adjust_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(adjust_panel);
  } else {
    lv_obj_add_flag(adjust_panel, LV_OBJ_FLAG_HIDDEN);
  }
}

static void adjust_reset(lv_event_t *e) {
  show_message("기본값으로 되돌리는 중...", 0);
  lv_refr_now(NULL);
  camera_ctl_reset();
  show_message("기본값으로 되돌렸습니다", 1500);
  show_adjust_values();
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, intptr_t code) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 30, 28);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_pad_all(btn, 0, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A4350), 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)code);
  lv_obj_t *l = lv_label_create(btn);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_obj_center(l);
  return btn;
}

// 미리보기 왼쪽 반에 덮는 목록: 이름, -, 값, +. 맨 위에 기본값으로 되돌리기
static void build_adjust() {
  adjust_panel = lv_obj_create(overlay);
  lv_obj_remove_style_all(adjust_panel);
  lv_obj_set_style_bg_color(adjust_panel, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(adjust_panel, LV_OPA_80, 0);
  lv_obj_set_style_pad_all(adjust_panel, 6, 0);
  lv_obj_set_style_pad_row(adjust_panel, 4, 0);
  lv_obj_set_flex_flow(adjust_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(adjust_panel, LV_DIR_VER);
  lv_obj_clear_flag(adjust_panel, LV_OBJ_FLAG_GESTURE_BUBBLE);  // 목록을 밀어도 카메라가 닫히지 않게
  lv_obj_add_flag(adjust_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *reset = lv_btn_create(adjust_panel);
  lv_obj_set_style_shadow_width(reset, 0, 0);
  lv_obj_set_style_bg_color(reset, lv_color_hex(0x5B6573), 0);
  lv_obj_add_event_cb(reset, adjust_reset, LV_EVENT_CLICKED, NULL);
  lv_obj_t *rl = lv_label_create(reset);
  lv_obj_set_style_text_font(rl, &font_ko_14, 0);
  lv_label_set_text(rl, "모두 기본값으로");
  int n = camera_ctl_count();
  adjust_vals = (lv_obj_t **)malloc(n * sizeof(lv_obj_t *));
  for (int i = 0; i < n; i++) {
    lv_obj_t *name = lv_label_create(adjust_panel);
    lv_obj_set_style_text_font(name, &font_ko_14, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xB8C0CA), 0);
    lv_label_set_text(name, camera_ctl_name(i));
    lv_obj_t *row = lv_obj_create(adjust_panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 28);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(small_button(row, LV_SYMBOL_MINUS, adjust_step, i * 2), LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(small_button(row, LV_SYMBOL_PLUS, adjust_step, i * 2 + 1), LV_ALIGN_RIGHT_MID, 0, 0);
    adjust_vals[i] = lv_label_create(row);
    lv_obj_set_style_text_font(adjust_vals[i], &font_ko_14, 0);
    lv_obj_set_style_text_color(adjust_vals[i], lv_color_white(), 0);
    lv_obj_center(adjust_vals[i]);
  }
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
  // 밀기 신호가 위 레이어(lv_layer_top)로 올라가지 않고 이 화면에서 받게 한다
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(overlay, overlay_gesture, LV_EVENT_GESTURE, NULL);

  preview = lv_img_create(overlay);

  guide = lv_obj_create(overlay);
  lv_obj_remove_style_all(guide);
  lv_obj_set_style_border_color(guide, lv_color_white(), 0);
  lv_obj_set_style_border_width(guide, 2, 0);
  lv_obj_set_style_border_opa(guide, LV_OPA_70, 0);
  lv_obj_set_style_radius(guide, 6, 0);
  lv_obj_clear_flag(guide, LV_OBJ_FLAG_CLICKABLE);
  guide_lbl = lv_label_create(overlay);
  lv_obj_set_style_text_font(guide_lbl, &font_ko_14, 0);
  lv_obj_set_style_text_color(guide_lbl, lv_color_white(), 0);
  lv_obj_set_style_bg_color(guide_lbl, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(guide_lbl, LV_OPA_50, 0);
  lv_obj_set_style_pad_hor(guide_lbl, 4, 0);
  lv_label_set_text(guide_lbl, "QR/바코드는 네모 안에 (20cm 이상)");

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
  // 오른쪽 위: SD 카드의 사진·동영상 보기 (gallery.h)
  gallery_btn = round_button(overlay, LV_SYMBOL_IMAGE, [](lv_event_t *e) { open_gallery(); });
  // 왼쪽 아래: 화질 조절 (화이트밸런스, 노출 등)
  adjust_btn = round_button(overlay, LV_SYMBOL_SETTINGS, adjust_toggle);
  // 촬영 버튼 옆(녹화 버튼 반대쪽): 연속 스캔 (QR·바코드를 찾으면 멈춘다)
  scan_btn = round_button(overlay, "QR", scan_clicked);
  lv_obj_set_size(scan_btn, 44, 44);
  build_adjust();

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
    lv_obj_align(scan_btn, LV_ALIGN_RIGHT_MID, -18, -70);
  } else {
    lv_obj_align(shutter, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_align(record_btn, LV_ALIGN_BOTTOM_MID, 70, -18);
    lv_obj_align(scan_btn, LV_ALIGN_BOTTOM_MID, -70, -18);
  }
  lv_obj_align(rec_lbl, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_align(send_lbl, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 6, 6);
  lv_obj_align(gallery_btn, LV_ALIGN_TOP_RIGHT, -6, 6);
  lv_obj_align(adjust_btn, LV_ALIGN_BOTTOM_LEFT, 6, -6);
  // 조절 목록: 닫기 버튼 아래부터 조절 버튼 위까지, 가로 화면은 왼쪽 반, 세로 화면은 폭 전부
  lv_obj_set_size(adjust_panel, w > h ? w / 2 : w - 12, h - 48 - 48);
  lv_obj_set_pos(adjust_panel, w > h ? 0 : 6, 48);
  lv_obj_center(msg_lbl);
  lv_obj_center(preview);
  // 가로 화면은 바코드처럼 넓게, 세로 화면은 폭에 맞춰
  lv_obj_set_size(guide, w > h ? w * 45 / 100 : w * 70 / 100, w > h ? h * 35 / 100 : h * 22 / 100);
  lv_obj_center(guide);
  lv_obj_align_to(guide_lbl, guide, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
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
  const char *name;
  uint32_t count = photo_finished(&ok, &ms, &name);
  if (count != seen) {
    seen = count;
    // 갤러리에서 다시 보낸 동영상(.mov)도 같은 길로 간다
    const char *dot = strrchr(name, '.');
    bool movie = dot && strcasecmp(dot, ".mov") == 0;
    if (ms > 2000 && movie) ui_toast(ok ? "동영상을 PC에 보냈습니다" : "동영상을 PC에 보내지 못했습니다", 3000);
    else if (ms > 2000) ui_toast(ok ? "사진을 PC에 보냈습니다" : "사진을 PC에 보내지 못했습니다", 2500);
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

// 카메라는 켜 두면 뜨거워지므로, 녹화 중이 아닐 때 이만큼 만지지 않으면 닫는다
#define CAMERA_IDLE_MS (2 * 60 * 1000UL)

void camera_ui_loop() {
  show_send_progress();
  if (!is_open) return;
  job_finish();
  scan_step();
  if (millis() - temp_at > 5000) {
    temp_at = millis();
    float t = temperatureRead();
    if (millis() - temp_log_at > 30000) {
      temp_log_at = millis();
      Serial.printf("[camera] chip %.1fC\n", t);
    }
    if (t >= CAMERA_HOT_C) {
      Serial.printf("[camera] too hot (%.1fC), closing\n", t);
      too_hot = true;
      close_camera();
      char msg[64];
      snprintf(msg, sizeof(msg), "기기가 뜨거워 카메라를 껐습니다\n(%.0f도, 식으면 다시 켜집니다)", t);
      ui_toast(msg, 4000);
      return;
    }
  }
  if (!video_recording() && millis() - display_last_touch_ms() > CAMERA_IDLE_MS) {
    Serial.println("[camera] idle, closing");
    close_camera();
    ui_toast("카메라를 2분 동안 안 써서 껐습니다\n(열이 나지 않게)", 3000);
    return;
  }
  // 스캔으로 읽은 뒤 그대로 두면 2분이 아니라 20초 만에 닫는다 (그 뒤에 만졌으면 평소대로)
  if (scan_done_at && !scanning && display_last_touch_ms() < scan_done_at &&
      millis() - scan_done_at > SCAN_DONE_IDLE_MS) {
    Serial.println("[camera] scan done, closing");
    close_camera();
    return;
  }
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
    // PC 로만 녹화하는 중에 PC 연결이 끊기면 멈춘다 (SD 카드에도 쓰는 중이면 계속한다)
    if (video_needs_pc() && link_active() != LINK_USB && link_active() != LINK_WIFI) stop_recording();
    // SD 카드가 가득 차 더 쓰지 못하면, PC 로도 보내는 중이 아니면 멈춘다 (알림은 sd_video 가 띄운다)
    if (sd_video_failed() && link_active() != LINK_USB && link_active() != LINK_WIFI) stop_recording();
  }
  if (job != JOB_NONE) return;  // 작업 태스크가 카메라를 쓰는 중
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

bool camera_ui_command(const char *cmd) {
  // 갤러리는 카메라 화면 위에 덮이므로, 열린 동안 켜면 카메라가 안 보이는 채로 돈다
  bool needs_camera = strcmp(cmd, "open") == 0 || strcmp(cmd, "shoot") == 0 || strcmp(cmd, "record") == 0 ||
                      strcmp(cmd, "scan") == 0;
  if (needs_camera && gallery_open()) {
    ui_toast("갤러리를 닫은 뒤에 다시 해 주세요", 3000);
    return false;
  }
  if (strcmp(cmd, "open") == 0) {
    open_camera();
    return camera_running();
  } else if (strcmp(cmd, "shoot") == 0) {
    open_camera();
    return take_photo();
  } else if (strcmp(cmd, "scan") == 0) {
    open_camera();
    if (!scanning) scan_clicked(nullptr);
    return scanning;
  } else if (strcmp(cmd, "dump") == 0 && frame_dsc.header.w) {
    // 진단용: 화면에 그리고 있는 미리보기를 그대로 사진으로 보낸다 (PC: --camera dump)
    int w = frame_dsc.header.w, h = frame_dsc.header.h;
    uint8_t *be = (uint8_t *)ps_malloc(w * h * 2);
    if (!be) return false;
    for (int i = 0; i < w * h; i++) {
      be[2 * i] = frame_buf[i] >> 8;
      be[2 * i + 1] = frame_buf[i] & 0xFF;
    }
    uint8_t *jpg;
    size_t len;
    bool converted = fmt2jpg(be, w * h * 2, w, h, PIXFORMAT_RGB565, 90, &jpg, &len);
    if (converted) photo_save(jpg, len, false);
    free(be);
    return converted;
  } else if (strcmp(cmd, "record") == 0) {
    open_camera();
    if (video_recording()) {
      stop_recording();
      return !video_recording();
    } else {
      start_recording();
      return video_recording();
    }
  } else if (strcmp(cmd, "close") == 0) {
    close_camera();
    return !camera_running();
  } else if (strcmp(cmd, "gallery") == 0) {
    if (is_open) {
      open_gallery();
    } else {
      gallery_show(nullptr);
    }
    return gallery_open();
  }
  return false;
}

void camera_ui_show() {
  open_camera();
}
