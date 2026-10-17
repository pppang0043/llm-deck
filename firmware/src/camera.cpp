#include "camera.h"

#include <Preferences.h>
#include <esp_camera.h>
#include <freertos/idf_additions.h>
#include <img_converters.h>

#include "pins.h"

static bool running = false;
static char model[16] = "";
static Preferences prefs;
// 처음 꽂은 카메라(OV5640)로 찍어 보고 정한 기본값. 다르면 PC 도우미로 바꿀 수 있다.
static uint8_t mount_rot = 0;
static bool mount_mirror = true;
static framesize_t preview_size = FRAMESIZE_QVGA;
static bool video_mode = false;
static esp_err_t last_error = ESP_OK;

// 카메라는 켤 때 내부 RAM 에서 16KB 짜리 연속 덩어리(DMA)가 필요하다. 오래 켜 두면 내부 RAM 이
// 잘게 쪼개져 이 덩어리를 못 얻을 수 있으므로, 부팅 때 자리를 미리 잡아 두었다가 카메라를 켜기
// 직전에 내놓고, 끄면 다시 잡는다.
#define RESERVE_BYTES (20 * 1024)
static void *reserve = nullptr;

static void hold_reserve() {
  if (!reserve) reserve = heap_caps_malloc(RESERVE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

static void release_reserve() {
  heap_caps_free(reserve);
  reserve = nullptr;
}

void camera_lend_reserve(bool lend) {
  // 카메라가 켜져 있으면 자리는 이미 카메라가 쓰고 있다
  if (running) return;
  if (lend) {
    release_reserve();
  } else {
    hold_reserve();
  }
}

// 카메라 센서 전원 끄기(PWDN 핀을 높게). 드라이버를 내려도 센서는 켜진 채 남아 열이 나므로 직접 끈다.
// 켤 때는 드라이버(esp_camera_init)가 이 핀을 낮게 내려 다시 켠다.
static void sensor_power_down() {
  pinMode(CAM_PWDN, OUTPUT);
  digitalWrite(CAM_PWDN, HIGH);
}

void camera_init() {
  sensor_power_down();
  hold_reserve();
  Serial.printf("[camera] reserved %s\n", reserve ? "ok" : "failed");
}
// JPEG 미리보기를 풀어 놓는 곳 (돌리기 전)
static uint8_t *decoded = nullptr;

// JPEG 머리(SOF)에 적힌 실제 가로·세로. 카메라 크기를 바꾼 직후에는 드라이버가 알려 주는 크기와
// 실제 그림 크기가 다를 수 있어서(이전 크기 그림이 몇 장 더 나온다) 그림에서 직접 읽는다.
static bool jpeg_size(const uint8_t *d, size_t len, int *w, int *h) {
  size_t i = 2;
  while (i + 9 < len && d[i] == 0xFF) {
    uint8_t marker = d[i + 1];
    size_t seg = (d[i + 2] << 8) | d[i + 3];
    if (marker >= 0xC0 && marker <= 0xC2) {
      *h = (d[i + 5] << 8) | d[i + 6];
      *w = (d[i + 7] << 8) | d[i + 8];
      return true;
    }
    i += 2 + seg;
  }
  return false;
}

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
  release_reserve();
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    sensor_power_down();
    hold_reserve();
    last_error = err;
    Serial.printf("[camera] start failed (0x%x), internal largest block %u\n", err,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return false;
  }
  last_error = ESP_OK;
  sensor_t *s = esp_camera_sensor_get();
  camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
  strlcpy(model, info ? info->name : "?", sizeof(model));
  s->set_hmirror(s, mount_mirror);
  preview_size = FRAMESIZE_QVGA;
  video_mode = false;
  s->set_framesize(s, preview_size);
  if (!decoded) decoded = (uint8_t *)ps_malloc(320 * 240 * 2);
  running = true;
  Serial.printf("[camera] %s on (mount rot %d, mirror %d), internal free %u, largest %u\n", model, mount_rot,
                mount_mirror, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  return true;
}

void camera_stop() {
  if (!running) return;
  camera_set_video(false, nullptr);
  esp_camera_deinit();
  sensor_power_down();
  running = false;
  hold_reserve();
  Serial.printf("[camera] off, internal free %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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

uint8_t camera_mount_rot() {
  load_mount();
  return mount_rot;
}

bool camera_mount_mirror() {
  load_mount();
  return mount_mirror;
}

uint8_t camera_turns(uint8_t screen_rot) {
  load_mount();
  return (mount_rot - screen_rot) & 3;
}

// 센서 영상을 시계 방향으로 몇 번 돌려야 지금 화면에서 바로 보이는지
static uint8_t turns_for(uint8_t screen_rot) {
  return (mount_rot - screen_rot) & 3;
}

// 녹화 중에는 따로 돌리는 태스크가 카메라에서 최대한 빨리 받아 넘기고(on_frame),
// 화면 미리보기는 가장 최근 장(latest)을 가져다 줄여서 그린다.
static void (*on_frame)(const uint8_t *, size_t) = nullptr;
static volatile bool capture_run = false;
static TaskHandle_t capture_task_handle = nullptr;
static SemaphoreHandle_t latest_lock = nullptr;
static uint8_t *latest = nullptr;
static size_t latest_len = 0;
static volatile bool latest_new = false;
#define LATEST_MAX (200 * 1024)

// 녹화 태스크는 한 번만 만들어 두고 켜고 끄기만 한다. 내부 RAM 이 빠듯해 스택은 PSRAM 에 둔다.
static volatile bool capture_active = false;

static void capture_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    capture_active = true;
    while (capture_run) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) continue;
      int jw = 0, jh = 0;
      // 크기를 바꾼 직후에는 이전 크기 그림이 섞여 나오므로 실제로 640 인 장만 쓴다
      if (fb->format == PIXFORMAT_JPEG && jpeg_size(fb->buf, fb->len, &jw, &jh) && jw == 640) {
        if (on_frame) on_frame(fb->buf, fb->len);
        // 화면이 앞 장을 그리는 중이면 이번 장은 미리보기에 쓰지 않는다
        if (fb->len <= LATEST_MAX && xSemaphoreTake(latest_lock, 0) == pdTRUE) {
          memcpy(latest, fb->buf, fb->len);
          latest_len = fb->len;
          latest_new = true;
          xSemaphoreGive(latest_lock);
        }
      }
      esp_camera_fb_return(fb);
    }
    capture_active = false;
  }
}

void camera_set_video(bool on, void (*frame_cb)(const uint8_t *jpg, size_t len)) {
  if (!running || on == video_mode) return;
  sensor_t *s = esp_camera_sensor_get();
  if (on) {
    if (!latest_lock) latest_lock = xSemaphoreCreateMutex();
    if (!latest) latest = (uint8_t *)ps_malloc(LATEST_MAX);
    on_frame = frame_cb;
    // 녹화 중에는 640x480 으로 찍는다
    s->set_framesize(s, FRAMESIZE_VGA);
    video_mode = true;
    latest_new = false;
    capture_run = true;
    if (!capture_task_handle) {
      xTaskCreatePinnedToCoreWithCaps(capture_task, "capture", 4096, NULL, 4, &capture_task_handle, 0, MALLOC_CAP_SPIRAM);
    }
    xTaskNotifyGive(capture_task_handle);
  } else {
    capture_run = false;
    while (capture_active) delay(5);
    on_frame = nullptr;
    video_mode = false;
    s->set_framesize(s, preview_size);
  }
}

bool camera_preview(uint16_t *out, int fit_w, int fit_h, uint8_t screen_rot, int *w, int *h) {
  if (!running || !decoded) return false;
  uint8_t k = turns_for(screen_rot);
  // 돌린 뒤 화면에 들어가는지: 320x240 이 안 들어가면 더 작게
  bool fits = (k & 1) ? (240 <= fit_w && 320 <= fit_h) : (320 <= fit_w && 240 <= fit_h);
  sensor_t *s = esp_camera_sensor_get();
  if (!video_mode) {
    framesize_t want = fits ? FRAMESIZE_QVGA : FRAMESIZE_HQVGA;
    if (want != preview_size) {
      s->set_framesize(s, want);
      preview_size = want;
    }
  }

  camera_fb_t *fb = nullptr;
  const uint8_t *jpg;
  size_t jpg_len;
  int sw, sh;
  if (video_mode) {
    // 녹화 태스크가 넣어 둔 가장 최근 장 (640x480)
    if (!latest_new || xSemaphoreTake(latest_lock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    latest_new = false;
    jpg = latest;
    jpg_len = latest_len;
  } else {
    fb = esp_camera_fb_get();
    if (!fb) return false;
    if (fb->format != PIXFORMAT_JPEG) {
      esp_camera_fb_return(fb);
      return false;
    }
    jpg = fb->buf;
    jpg_len = fb->len;
  }
  bool sized = jpeg_size(jpg, jpg_len, &sw, &sh);
  // 녹화 중(640x480)에는 풀 때 1/2 (안 들어가면 1/4) 로 줄인다
  esp_jpeg_image_scale_t scale = JPG_SCALE_NONE;
  if (sized && sw > 320) {
    int div = sw / 320;  // 640 -> 2, 1600 -> 5 (안 맞으면 아래에서 거른다)
    if (!fits) div *= 2;
    if (div <= 2) {
      scale = JPG_SCALE_2X;
      div = 2;
    } else if (div <= 4) {
      scale = JPG_SCALE_4X;
      div = 4;
    } else {
      scale = JPG_SCALE_8X;
      div = 8;
    }
    sw /= div;
    sh /= div;
  }
  // 다 풀었을 때 준비한 버퍼(320x240)에 들어갈 때만 푼다
  bool ok = sized && sw * sh <= 320 * 240 && jpg2rgb565(jpg, jpg_len, decoded, scale);
  if (fb) {
    esp_camera_fb_return(fb);
  } else {
    xSemaphoreGive(latest_lock);
  }
  if (!ok) {
    static uint32_t bad = 0;
    if (++bad % 20 == 1) Serial.printf("[camera] preview decode failed (%dx%d, %u total)\n", sw, sh, (unsigned)bad);
    return false;
  }

  // 풀어 놓은 그림은 이미 화면과 같은 바이트 순서다. k 번 돌리기만 한다.
  int ow = (k & 1) ? sh : sw, oh = (k & 1) ? sw : sh;
  for (int y = 0; y < sh; y++) {
    for (int x = 0; x < sw; x++) {
      const uint8_t *p = decoded + 2 * (y * sw + x);
      uint16_t c = p[0] | (p[1] << 8);
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
  // 큰 크기로 나온 첫 장을 쓴다 (보통 2~3장 뒤)
  camera_fb_t *fb = nullptr;
  for (int i = 0; i < 10; i++) {
    fb = esp_camera_fb_get();
    int jw = 0, jh = 0;
    if (fb && jpeg_size(fb->buf, fb->len, &jw, &jh) && jw == 1600 && i >= 1) break;
    if (fb) esp_camera_fb_return(fb);
    fb = nullptr;
  }
  bool ok = false;
  if (fb && fb->format == PIXFORMAT_JPEG) {
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

bool camera_missing() {
  // 카메라가 응답하지 않았을 때(꽂혀 있지 않음)와 메모리가 모자랄 때를 구별한다
  return last_error == ESP_ERR_NOT_FOUND || last_error == ESP_ERR_NOT_SUPPORTED;
}
