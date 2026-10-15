#include "camera.h"

#include <esp_camera.h>
#include <img_converters.h>

#include "pins.h"

static bool running = false;
static bool landscape_now = true;
static char model[16] = "";

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

static bool init_preview() {
  camera_config_t config = base_config();
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QVGA;
  config.fb_count = 2;
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[camera] not found (0x%x)\n", err);
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (!landscape_now) s->set_framesize(s, FRAMESIZE_HQVGA);
  camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
  strlcpy(model, info ? info->name : "?", sizeof(model));
  return true;
}

bool camera_start(bool landscape) {
  landscape_now = landscape;
  if (running) {
    camera_set_landscape(landscape);
    return true;
  }
  if (!init_preview()) return false;
  running = true;
  Serial.printf("[camera] %s preview on\n", model);
  return true;
}

void camera_set_landscape(bool landscape) {
  landscape_now = landscape;
  if (!running) return;
  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, landscape ? FRAMESIZE_QVGA : FRAMESIZE_HQVGA);
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

bool camera_preview(uint16_t *out, size_t max_pixels, int *w, int *h) {
  if (!running) return false;
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;
  size_t pixels = fb->width * fb->height;
  bool ok = fb->format == PIXFORMAT_RGB565 && pixels <= max_pixels && fb->len >= pixels * 2;
  if (ok) {
    // 카메라는 큰 쪽 바이트를 먼저 보내고, 화면(LVGL)은 작은 쪽을 먼저 쓴다
    const uint8_t *src = fb->buf;
    for (size_t i = 0; i < pixels; i++) out[i] = (src[2 * i] << 8) | src[2 * i + 1];
    *w = fb->width;
    *h = fb->height;
  }
  esp_camera_fb_return(fb);
  return ok;
}

bool camera_capture(uint8_t **jpg, size_t *len) {
  if (!running) return false;
  // 미리보기(RGB565)를 끄고 큰 JPEG 로 다시 켜서 찍은 뒤, 미리보기로 돌아간다
  esp_camera_deinit();
  camera_config_t config = base_config();
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  bool ok = false;
  if (esp_camera_init(&config) == ESP_OK) {
    // 처음 몇 장은 밝기 자동 조절이 덜 되어 어둡게 나오므로 버린다
    for (int i = 0; i < 4; i++) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) esp_camera_fb_return(fb);
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb && fb->format == PIXFORMAT_JPEG) {
      *jpg = (uint8_t *)ps_malloc(fb->len);
      if (*jpg) {
        memcpy(*jpg, fb->buf, fb->len);
        *len = fb->len;
        ok = true;
      }
    }
    if (fb) esp_camera_fb_return(fb);
    esp_camera_deinit();
  }
  running = init_preview();
  Serial.printf("[camera] capture %s (%u bytes)\n", ok ? "ok" : "failed", ok ? (unsigned)*len : 0);
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
