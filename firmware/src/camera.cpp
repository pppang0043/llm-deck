#include "camera.h"

#include <Preferences.h>
#include <esp_camera.h>
#include <img_converters.h>

#include "pins.h"

static bool running = false;
static char model[16] = "";
static Preferences prefs;
// 처음 꽂은 카메라(OV5640)로 찍어 보고 정한 기본값. 다르면 PC 도우미로 바꿀 수 있다.
static uint8_t mount_rot = 1;
static bool mount_mirror = false;
static framesize_t preview_size = FRAMESIZE_QVGA;
// JPEG 미리보기를 풀어 놓는 곳 (돌리기 전)
static uint8_t *decoded = nullptr;

static camera_config_t base_config() {
  camera_config_t config = {};
  config.pin_pwdn = CAM_PWDN;
  config.pin_reset = -1;
  config.pin_xclk = CAM_XCLK;
  config.pin_sccb_sda = CAM_SIOD;
  config.pin_sccb_scl = CAM_SIOC;
  config.pin_d7 = CAM_D7;
  config.pin_d6 = CAM_D6;
  config.pin_d5 = CAM_D5;
  config.pin_d4 = CAM_D4;
  config.pin_d3 = CAM_D3;
  config.pin_d2 = CAM_D2;
  config.pin_d1 = CAM_D1;
  config.pin_d0 = CAM_D0;
  config.pin_vsync = CAM_VSYNC;
  config.pin_href = CAM_HREF;
  config.pin_pclk = CAM_PCLK;
  config.xclk_freq_hz = 20000000;
  // 화면 밝기(백라이트)가 LEDC 앞번호를 쓰므로 카메라 클럭은 뒤쪽 번호를 쓴다
  config.ledc_timer = LEDC_TIMER_3;
  config.ledc_channel = LEDC_CHANNEL_7;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;
  return config;
}

static void load_mount() {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;
  prefs.begin("llm-deck");
  mount_rot = prefs.getUChar("cam_rot", mount_rot) & 3;
  mount_mirror = prefs.getBool("cam_mirror", mount_mirror);
}

bool camera_start() {
  if (running) return true;
  load_mount();
  camera_config_t config = base_config();
  // 버퍼는 가장 큰 사진 크기로 잡아 두고, 미리보기 동안에는 작게 찍는다
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 10;
  config.fb_count = 2;
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[camera] not found (0x%x)\n", err);
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
  strlcpy(model, info ? info->name : "?", sizeof(model));
  s->set_hmirror(s, mount_mirror);
  preview_size = FRAMESIZE_QVGA;
  s->set_framesize(s, preview_size);
  if (!decoded) decoded = (uint8_t *)ps_malloc(320 * 240 * 2);
  running = true;
  Serial.printf("[camera] %s on (mount rot %d, mirror %d)\n", model, mount_rot, mount_mirror);
  return true;
}

void camera_stop() {
  if (!running) return;
  esp_camera_deinit();
  running = false;
  Serial.println("[camera] off");
}

bool camera_running() {
  return running;
}

const char *camera_model() {
  return model;
}

void camera_set_mount(uint8_t rot, bool mirror) {
  load_mount();
  mount_rot = rot & 3;
  mount_mirror = mirror;
  prefs.putUChar("cam_rot", mount_rot);
  prefs.putBool("cam_mirror", mount_mirror);
  if (running) {
    sensor_t *s = esp_camera_sensor_get();
    s->set_hmirror(s, mount_mirror);
  }
  Serial.printf("[camera] mount rot %d, mirror %d\n", mount_rot, mount_mirror);
}

// 센서 영상을 시계 방향으로 몇 번 돌려야 지금 화면에서 바로 보이는지
static uint8_t turns_for(uint8_t screen_rot) {
  return (mount_rot - screen_rot) & 3;
}

bool camera_preview(uint16_t *out, int fit_w, int fit_h, uint8_t screen_rot, int *w, int *h) {
  if (!running || !decoded) return false;
  uint8_t k = turns_for(screen_rot);
  // 돌린 뒤 화면에 들어가도록 센서 크기를 고른다: 320x240 이 안 들어가면 240x176
  bool fits = (k & 1) ? (240 <= fit_w && 320 <= fit_h) : (320 <= fit_w && 240 <= fit_h);
  framesize_t want = fits ? FRAMESIZE_QVGA : FRAMESIZE_HQVGA;
  sensor_t *s = esp_camera_sensor_get();
  if (want != preview_size) {
    s->set_framesize(s, want);
    preview_size = want;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;
  int sw = fb->width, sh = fb->height;
  bool ok = fb->format == PIXFORMAT_JPEG && sw * sh <= 320 * 240 &&
            jpg2rgb565(fb->buf, fb->len, decoded, JPG_SCALE_NONE);
  esp_camera_fb_return(fb);
  if (!ok) return false;

  // 풀어 놓은 그림(큰 쪽 바이트 먼저)을 화면 바이트 순서로 바꾸면서 k 번 돌린다
  int ow = (k & 1) ? sh : sw, oh = (k & 1) ? sw : sh;
  for (int y = 0; y < sh; y++) {
    for (int x = 0; x < sw; x++) {
      const uint8_t *p = decoded + 2 * (y * sw + x);
      uint16_t c = (p[0] << 8) | p[1];
      int ox, oy;
      switch (k) {
        case 1: ox = sh - 1 - y; oy = x; break;
        case 2: ox = sw - 1 - x; oy = sh - 1 - y; break;
        case 3: ox = y; oy = sw - 1 - x; break;
        default: ox = x; oy = y; break;
      }
      out[oy * ow + ox] = c;
    }
  }
  *w = ow;
  *h = oh;
  return true;
}

// JPEG 맨 앞(SOI 다음)에 회전 정보(EXIF Orientation)를 넣는다. 사진 앱들이 이 값대로 돌려서 보여준다.
static uint8_t *add_orientation(const uint8_t *jpg, size_t len, uint8_t k, size_t *out_len) {
  static const uint16_t ORIENT[4] = {1, 6, 3, 8};  // 시계 방향 0/90/180/270
  const uint8_t app1[] = {
      0xFF, 0xE1, 0x00, 34,                         // APP1, 길이 34
      'E', 'x', 'i', 'f', 0, 0,                     // Exif 표시
      'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, // TIFF 머리 (작은 쪽 먼저)
      0x01, 0x00,                                   // 항목 1개
      0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, // Orientation, SHORT, 1개
      (uint8_t)ORIENT[k], 0x00, 0x00, 0x00,         // 값
      0x00, 0x00, 0x00, 0x00,                       // 다음 IFD 없음
  };
  if (len < 2 || jpg[0] != 0xFF || jpg[1] != 0xD8) return nullptr;
  uint8_t *out = (uint8_t *)ps_malloc(len + sizeof(app1));
  if (!out) return nullptr;
  out[0] = 0xFF;
  out[1] = 0xD8;
  memcpy(out + 2, app1, sizeof(app1));
  memcpy(out + 2 + sizeof(app1), jpg + 2, len - 2);
  *out_len = len + sizeof(app1);
  return out;
}

bool camera_capture(uint8_t screen_rot, uint8_t **jpg, size_t *len) {
  if (!running) return false;
  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_UXGA);
  // 크기를 바꾼 직전 몇 장은 이전 크기가 섞여 나오므로 버린다
  camera_fb_t *fb = nullptr;
  for (int i = 0; i < 3; i++) {
    fb = esp_camera_fb_get();
    if (i < 2 && fb) {
      esp_camera_fb_return(fb);
      fb = nullptr;
    }
  }
  bool ok = false;
  if (fb && fb->format == PIXFORMAT_JPEG && fb->width == 1600) {
    *jpg = add_orientation(fb->buf, fb->len, turns_for(screen_rot), len);
    ok = *jpg != nullptr;
  }
  if (fb) esp_camera_fb_return(fb);
  s->set_framesize(s, preview_size);
  Serial.printf("[camera] capture %s (%u bytes, screen rot %d, turns %d)\n", ok ? "ok" : "failed",
                ok ? (unsigned)*len : 0, screen_rot, turns_for(screen_rot));
  return ok;
}

bool camera_test_pattern(uint8_t **jpg, size_t *len) {
  // 실제 사진과 비슷한 크기(100KB 안팎)가 되도록 크게, 잔무늬를 섞어 만든다
  const int w = 800, h = 600;
  uint8_t *rgb = (uint8_t *)ps_malloc(w * h * 2);
  if (!rgb) return false;
  // 색 띠 + 격자 무늬
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint8_t noise = (x * 7 + y * 13 + (x * y) % 11) & 7;
      uint8_t r = x * 31 / w, g = (y * 63 / h) ^ noise, b = ((x / 100 + y / 100) & 1) ? 31 : 8;
      uint16_t c = (r << 11) | (g << 5) | b;
      rgb[2 * (y * w + x)] = c >> 8;
      rgb[2 * (y * w + x) + 1] = c & 0xFF;
    }
  }
  bool ok = fmt2jpg(rgb, w * h * 2, w, h, PIXFORMAT_RGB565, 85, jpg, len);
  free(rgb);
  return ok;
}
