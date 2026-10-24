#include "camera.h"

#include <Preferences.h>
#include <ESP32_OV5640_cfg.h>  // OV5640AF 초점 펌웨어 (OV5640_AF_Config) 와 명령 레지스터
#include <esp_camera.h>
#include <esp_task_wdt.h>
#include <freertos/idf_additions.h>
#include <img_converters.h>

#include "pins.h"

static bool running = false;
static char model[16] = "";
static Preferences prefs;
// 카메라(OV5640)로 찍어 보고 정한 기본값 (2026-10-24 다시 끼운 뒤: 가로 화면(rot 1)에서 그대로가 바로 선다).
// 다르면 설정 > 카메라 나 PC 도우미(--camera-mount)로 바꿀 수 있다.
static uint8_t mount_rot = 1;
static bool mount_mirror = true;
static framesize_t preview_size = FRAMESIZE_QVGA;
// 사진은 센서 최대(2560x1920)로: 바코드 막대 하나가 1600 일 때 3화소도 안 되어 읽히지 않았다 (2026-10-24 시험)
#define PHOTO_SIZE FRAMESIZE_QSXGA
#define PHOTO_W 2560
// 사진 크기일 때 센서 창 (켤 때 읽어 둔다). 스캔은 이 창의 가운데만 같은 배율로 읽는다
static int full_hts = 0, full_vts = 0, full_x0 = 0, full_y0 = 0, full_x1 = 0, full_y1 = 0, full_ox = 0, full_oy = 0;
static bool video_mode = false;
static bool scan_mode = false;  // 스캔 2단계 창(camera_scan_zoom)을 그대로 둔다
static esp_err_t last_error = ESP_OK;
// 자동 초점 (OV5640AF): 센서는 켤 때마다 초점 펌웨어가 지워진다. 4KB 를 한 바이트씩 넣느라 6~7초 걸리므로
// 따로 도는 태스크가 넣고, 그동안 미리보기는 그냥 돈다 (초점은 넣은 뒤부터).
static volatile bool af_on = false;       // 초점 펌웨어가 돌고 있다
static volatile bool af_loading = false;  // 넣는 중
static volatile bool af_abort = false;    // 넣는 중에 카메라를 끈다

// 명령 하나: ACK 를 1 로 두고 명령을 쓴 뒤, 센서가 ACK 를 0 으로 돌려놓을 때까지 기다린다
static bool af_command(sensor_t *s, uint8_t cmd) {
  s->set_reg(s, OV5640_CMD_ACK, 0xff, 0x01);
  s->set_reg(s, OV5640_CMD_MAIN, 0xff, cmd);
  for (int i = 0; i < 1000; i++) {  // 계속 맞추기(0x04)는 1초 넘게 걸린다. 라이브러리와 같게 5초까지
    if (s->get_reg(s, OV5640_CMD_ACK, 0xff) == 0x00) return true;
    // 찍을 때는 화면 루프에서 불린다: 기다리는 동안 멈춤 감시(5초)에 걸려 재부팅되지 않게 (2026-10-24)
    esp_task_wdt_reset();
    delay(5);
  }
  return false;
}

// 렌즈 위치 (VCM DAC 10비트). 초점을 맞출 때 바뀌어야 렌즈가 실제로 움직인 것이다
static int af_position(sensor_t *s) {
  return ((s->get_reg(s, 0x3603, 0x3f) << 4) | (s->get_reg(s, 0x3602, 0xf0) >> 4));
}

static void af_task(void *) {
  sensor_t *s = esp_camera_sensor_get();
  uint32_t t0 = millis();
  bool ok = s->get_reg(s, OV5640_CHIPID_HIGH, 0xff) == 0x56 && s->get_reg(s, OV5640_CHIPID_LOW, 0xff) == 0x40;
  if (ok) {
    s->set_reg(s, 0x3000, 0xff, 0x20);  // 초점 MCU 멈춤
    for (size_t i = 0; ok && i < sizeof(OV5640_AF_Config); i++) {
      if (af_abort || s->set_reg(s, 0x8000 + i, 0xff, OV5640_AF_Config[i]) < 0) ok = false;
    }
  }
  const char *step = ok ? "boot" : "load";  // 실패하면 어느 단계였는지 로그에 남긴다
  uint32_t loaded_ms = millis() - t0;
  int st = -1;
  if (ok) {
    for (uint16_t reg = OV5640_CMD_MAIN; reg <= OV5640_CMD_PARA4; reg++) s->set_reg(s, reg, 0xff, 0x00);
    s->set_reg(s, OV5640_CMD_FW_STATUS, 0xff, 0x7f);
    s->set_reg(s, 0x3000, 0xff, 0x00);  // 초점 MCU 시작
    ok = false;
    for (int i = 0; i < 1000 && !af_abort; i++) {  // 라이브러리와 같게 5초까지
      st = s->get_reg(s, OV5640_CMD_FW_STATUS, 0xff);
      if (st == FW_STATUS_S_IDLE) {
        ok = true;
        break;
      }
      delay(5);
    }
  }
  if (ok) step = "release";
  ok = ok && !af_abort && af_command(s, 0x08);  // 풀기
  if (ok) step = "continuous";
  ok = ok && !af_abort && af_command(s, AF_CONTINUE_AUTO_FOCUS);  // 계속 맞추기
  af_on = ok;
  Serial.printf("[camera] autofocus %s (%lu ms, load %lu ms, status 0x%02x, lens %d)\n",
                ok ? "on" : af_abort ? "stopped" : step, (unsigned long)(millis() - t0), (unsigned long)loaded_ms,
                st & 0xff, af_position(s));
  af_loading = false;
  vTaskDelete(nullptr);
}

static void af_start() {
  af_on = false;
  af_abort = false;
  af_loading = true;
  if (xTaskCreatePinnedToCoreWithCaps(af_task, "af", 4096, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
    af_loading = false;
  }
}

static void af_stop() {
  af_abort = true;
  while (af_loading) delay(5);  // 한 바이트 쓰는 사이에 멈추므로 곧 끝난다
  af_on = false;
}

// 찍기 직전: 사진 크기(PHOTO_SIZE)로 바꾼 뒤 한 번 맞추고 (최대 1.5초) 렌즈 위치를 적는다. 찍은 뒤에는 다시 계속 맞추기로.
// (렌즈를 직접 옮겨 훑어 보았지만 자리마다 선명도 차이가 없어 센서의 초점에 맡긴다)
static void af_focus_once(sensor_t *s, char *note, size_t size) {
  if (!af_on) {
    snprintf(note, size, af_loading ? "focus loading" : "no focus");
    return;
  }
  int before = af_position(s);
  uint32_t t0 = millis();
  uint8_t st = 0xFF;
  if (af_command(s, AF_TRIG_SINGLE_AUTO_FOCUS)) {
    while (millis() - t0 < 1500) {
      st = s->get_reg(s, OV5640_CMD_FW_STATUS, 0xff);
      if (st == FW_STATUS_S_FOCUSED) break;
      esp_task_wdt_reset();
      delay(20);
    }
  }
  snprintf(note, size, "focus 0x%02x in %lu ms, lens %d -> %d", st, (unsigned long)(millis() - t0), before,
           af_position(s));
}

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

static void apply_ctls(sensor_t *s);

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
  config.frame_size = PHOTO_SIZE;
  // 2560 사진은 어둡거나 잔무늬가 많으면 10 에서도 890KB 까지 커져 버퍼(약 980KB) 한계에 닿았다 (2026-10-24)
  config.jpeg_quality = 12;
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
  // 지금은 사진 크기(PHOTO_SIZE)로 켜져 있다: 센서 창·한 줄 길이 등을 읽어 둔다 (camera_scan_zoom)
  auto reg16 = [&](int r) { return (s->get_reg(s, r, 0xff) << 8) | s->get_reg(s, r + 1, 0xff); };
  full_x0 = reg16(0x3800), full_y0 = reg16(0x3802), full_x1 = reg16(0x3804), full_y1 = reg16(0x3806);
  full_ox = reg16(0x3810), full_oy = reg16(0x3812), full_hts = reg16(0x380C), full_vts = reg16(0x380E);
  Serial.printf("[camera] full window %d,%d-%d,%d offset %d,%d total %dx%d\n", full_x0, full_y0, full_x1, full_y1,
                full_ox, full_oy, full_hts, full_vts);
  camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
  strlcpy(model, info ? info->name : "?", sizeof(model));
  s->set_hmirror(s, mount_mirror);
  apply_ctls(s);
  af_start();
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
  af_stop();
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

// ---- 화질 조절 ----

// 드라이버(esp32-camera)의 set_* 하나. 값은 드라이버가 status 에 적어 둔 것을 읽는다.
// key 는 기기 저장 이름 ("cc_" + key, 15자 안)
struct CamCtl {
  const char *key;
  const char *name;
  int16_t lo, hi, step;
  int (*set)(sensor_t *, int);
  int (*get)(const camera_status_t &);
  const char *const *labels;  // 값마다 이름 (lo 부터). 없으면 숫자, 0~1 이면 끔/켬
};
static const char *const WB_MODES[] = {"자동", "맑음", "흐림", "사무실", "실내"};
static const char *const EFFECTS[] = {"없음", "반전", "흑백", "붉게", "초록", "푸르게", "세피아"};
static const char *const CEILINGS[] = {"2x", "4x", "8x", "16x", "32x", "64x", "128x"};
#define CTL(key, name, lo, hi, step, fn, field, labels)                                                   \
  {key, name, lo, hi, step, [](sensor_t *s, int v) { return s->fn(s, v); },                                 \
   [](const camera_status_t &st) { return (int)st.field; }, labels}
static const CamCtl CTLS[] = {
    CTL("awb", "자동 화이트밸런스", 0, 1, 1, set_whitebal, awb, nullptr),
    CTL("wb_mode", "화이트밸런스", 0, 4, 1, set_wb_mode, wb_mode, WB_MODES),
    CTL("awb_gain", "AWB 게인", 0, 1, 1, set_awb_gain, awb_gain, nullptr),
    CTL("aec", "자동 노출", 0, 1, 1, set_exposure_ctrl, aec, nullptr),
    CTL("aec2", "AEC2 (어두운 곳)", 0, 1, 1, set_aec2, aec2, nullptr),
    CTL("ae_level", "노출 보정", -2, 2, 1, set_ae_level, ae_level, nullptr),
    CTL("aec_value", "노출 (수동)", 0, 1200, 50, set_aec_value, aec_value, nullptr),
    CTL("agc", "자동 게인", 0, 1, 1, set_gain_ctrl, agc, nullptr),
    CTL("agc_gain", "게인 (수동)", 0, 30, 1, set_agc_gain, agc_gain, nullptr),
    {"gainceil", "게인 상한", 0, 6, 1, [](sensor_t *s, int v) { return s->set_gainceiling(s, (gainceiling_t)v); },
     [](const camera_status_t &st) { return (int)st.gainceiling; }, CEILINGS},
    CTL("bright", "밝기", -3, 3, 1, set_brightness, brightness, nullptr),
    CTL("contrast", "대비", -3, 3, 1, set_contrast, contrast, nullptr),
    CTL("satur", "채도", -4, 4, 1, set_saturation, saturation, nullptr),
    CTL("sharp", "선명도", -3, 3, 1, set_sharpness, sharpness, nullptr),
    CTL("denoise", "노이즈 줄이기", 0, 8, 1, set_denoise, denoise, nullptr),
    CTL("special", "효과", 0, 6, 1, set_special_effect, special_effect, EFFECTS),
    CTL("lenc", "렌즈 보정", 0, 1, 1, set_lenc, lenc, nullptr),
    CTL("raw_gma", "감마 보정", 0, 1, 1, set_raw_gma, raw_gma, nullptr),
    CTL("bpc", "불량 화소 (검정)", 0, 1, 1, set_bpc, bpc, nullptr),
    CTL("wpc", "불량 화소 (흰색)", 0, 1, 1, set_wpc, wpc, nullptr),
    CTL("dcw", "축소 (DCW)", 0, 1, 1, set_dcw, dcw, nullptr),
    CTL("quality", "JPEG 압축 (낮을수록 고화질)", 12, 63, 1, set_quality, quality, nullptr),
    CTL("colorbar", "컬러바 (시험)", 0, 1, 1, set_colorbar, colorbar, nullptr),
};
#undef CTL
#define CTL_COUNT (int)(sizeof(CTLS) / sizeof(CTLS[0]))

static void ctl_key(int i, char *out) {
  snprintf(out, 16, "cc_%s", CTLS[i].key);
}

// 켤 때마다: 저장해 둔 것만 다시 넣는다 (나머지는 드라이버 기본값)
static void apply_ctls(sensor_t *s) {
  for (int i = 0; i < CTL_COUNT; i++) {
    char key[16];
    ctl_key(i, key);
    if (!prefs.isKey(key)) continue;
    int v = prefs.getShort(key, 0);
    // 범위가 줄었으면 (압축 12 아래는 2560 사진이 버퍼를 넘을 수 있다) 저장값을 버린다
    if (v < CTLS[i].lo || v > CTLS[i].hi) {
      prefs.remove(key);
      continue;
    }
    int err = CTLS[i].set(s, v);
    Serial.printf("[camera] %s=%d%s\n", CTLS[i].key, v, err ? " (거부)" : "");
  }
}

int camera_ctl_count() {
  return CTL_COUNT;
}

const char *camera_ctl_name(int i) {
  return i >= 0 && i < CTL_COUNT ? CTLS[i].name : "";
}

bool camera_ctl_value(int i, int *v) {
  if (!running || i < 0 || i >= CTL_COUNT) return false;
  *v = CTLS[i].get(esp_camera_sensor_get()->status);
  return true;
}

bool camera_ctl_step(int i, int dir) {
  int v;
  if (!camera_ctl_value(i, &v)) return false;
  const CamCtl &c = CTLS[i];
  int next = constrain(v + dir * c.step, c.lo, c.hi);
  if (next == v) return false;
  sensor_t *s = esp_camera_sensor_get();
  int err = c.set(s, next);
  Serial.printf("[camera] set %s=%d%s\n", c.key, next, err ? " (거부)" : "");
  if (err) return false;
  char key[16];
  ctl_key(i, key);
  prefs.putShort(key, next);
  return true;
}

bool camera_ctl_set(const char *key, int v) {
  if (strcmp(key, "reset") == 0) {
    camera_ctl_reset();
    return true;
  }
  int i = 0;
  while (i < CTL_COUNT && strcmp(CTLS[i].key, key) != 0) i++;
  if (i == CTL_COUNT || v < CTLS[i].lo || v > CTLS[i].hi) {
    Serial.printf("[camera] set %s=%d: unknown or out of range\n", key, v);
    return false;
  }
  load_mount();
  if (running) {
    int err = CTLS[i].set(esp_camera_sensor_get(), v);
    Serial.printf("[camera] set %s=%d%s\n", key, v, err ? " (거부)" : "");
    if (err) return false;
  }
  char k[16];
  ctl_key(i, k);
  prefs.putShort(k, v);
  return true;
}

void camera_ctl_text(int i, int v, char *out, size_t size) {
  const CamCtl &c = CTLS[i];
  if (c.labels && v >= c.lo && v <= c.hi) {
    snprintf(out, size, "%s", c.labels[v - c.lo]);
  } else if (c.lo == 0 && c.hi == 1) {
    snprintf(out, size, "%s", v ? "켬" : "끔");
  } else {
    snprintf(out, size, "%d", v);
  }
}

void camera_ctl_reset() {
  load_mount();
  for (int i = 0; i < CTL_COUNT; i++) {
    char key[16];
    ctl_key(i, key);
    prefs.remove(key);
  }
  Serial.println("[camera] controls reset");
  // 드라이버 기본값은 센서를 처음부터 다시 켜야 돌아온다
  if (running) {
    camera_stop();
    camera_start();
  }
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
    // 찍던 장을 마칠 때까지 기다리되, 카메라가 대답하지 않으면 2초 뒤 그냥 넘어간다
    uint32_t waited = millis();
    while (capture_active && millis() - waited < 2000) delay(5);
    if (capture_active) Serial.println("[camera] capture did not stop in 2s");
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
  if (!video_mode && !scan_mode) {
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

bool camera_capture(uint8_t screen_rot, uint8_t **jpg, size_t *len, bool focus) {
  // 녹화 중에 크기를 바꾸면 녹화 크기로 돌아오지 않는다
  if (!running || video_mode) return false;
  sensor_t *s = esp_camera_sensor_get();
  uint32_t t0 = millis();
  s->set_framesize(s, PHOTO_SIZE);  // 초점도 사진 크기에서 맞춘다
  char note[64] = "focus kept";
  if (focus) af_focus_once(s, note, sizeof(note));
  // 크기를 바꾼 직전 몇 장은 이전 크기가 섞여 나오므로 버린다
  // 큰 크기로 나온 첫 장을 쓴다 (보통 2~3장 뒤)
  camera_fb_t *fb = nullptr;
  for (int i = 0; i < 10; i++) {
    esp_task_wdt_reset();  // 최대 크기는 한 장이 느리다
    fb = esp_camera_fb_get();
    int jw = 0, jh = 0;
    if (fb && jpeg_size(fb->buf, fb->len, &jw, &jh) && jw == PHOTO_W && i >= 1) break;
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
  if (af_on) af_command(s, AF_CONTINUE_AUTO_FOCUS);
  // 초점: 0x10 맞음, 0x00~0x0F·0x80~0x8F 맞추는 중, 0x70 쉼
  Serial.printf("[camera] capture %s (%u bytes, %lu ms, screen rot %d, turns %d, %s)\n", ok ? "ok" : "failed",
                ok ? (unsigned)*len : 0, (unsigned long)(millis() - t0), screen_rot, turns_for(screen_rot), note);
  return ok;
}

// ---- QR·바코드 스캔 (2단계: 넓게 보고 자리를 찾은 뒤, 그 자리만 원본 화소로) ----
static int roi_w = 0;  // 2단계 창의 출력 폭 (0 이면 2단계가 아니다)

// 폭이 w 인 장이 나올 때까지 (크기를 바꾼 직후의 앞 장은 skip 만큼 버린다). 못 받으면 nullptr
static camera_fb_t *frame_of_width(int w, int tries, int skip) {
  for (int i = 0; i < tries; i++) {
    esp_task_wdt_reset();
    camera_fb_t *fb = esp_camera_fb_get();
    int jw = 0, jh = 0;
    if (fb && i >= skip && jpeg_size(fb->buf, fb->len, &jw, &jh) && jw == w) return fb;
    if (fb) esp_camera_fb_return(fb);
  }
  return nullptr;
}

static bool take_frame(camera_fb_t *fb, uint8_t screen_rot, uint8_t **jpg, size_t *len) {
  bool ok = fb && fb->format == PIXFORMAT_JPEG &&
            (*jpg = add_orientation(fb->buf, fb->len, turns_for(screen_rot), len)) != nullptr;
  if (fb) esp_camera_fb_return(fb);
  return ok;
}

bool camera_scan_locate(uint8_t screen_rot, uint8_t **jpg, size_t *len) {
  if (!running || video_mode) return false;
  sensor_t *s = esp_camera_sensor_get();
  uint32_t t0 = millis();
  scan_mode = false;
  roi_w = 0;
  s->set_framesize(s, FRAMESIZE_XGA);  // 사진과 같은 화각을 작게 (2560 은 밝은 곳에서 버퍼를 넘었다)
  bool ok = take_frame(frame_of_width(1024, 8, 1), screen_rot, jpg, len);
  s->set_framesize(s, preview_size);
  Serial.printf("[camera] scan locate %s (%u bytes, %lu ms)\n", ok ? "ok" : "failed", ok ? (unsigned)*len : 0,
                (unsigned long)(millis() - t0));
  return ok;
}

// 2단계 창의 지금 장 JPEG 크기 (렌즈를 옮기는 동안 찍히던 장은 버린다)
static size_t roi_size() {
  camera_fb_t *fb = frame_of_width(roi_w, 3, 1);
  size_t len = fb ? fb->len : 0;
  if (fb) esp_camera_fb_return(fb);
  return len;
}

static void set_lens(sensor_t *s, int pos) {
  s->set_reg(s, 0x3603, 0x3f, pos >> 4);
  s->set_reg(s, 0x3602, 0xf0, (pos & 0x0f) << 4);
}

// 렌즈를 직접 옮겨 가며 창 안이 가장 선명한(JPEG 가 가장 큰) 자리에 둔다.
// 센서 자동 초점은 같은 장면에서도 자리가 들쭉날쭉했고, 줄인 장으로 재면 자리마다 차이가 보이지 않았다 (2026-10-24)
static void sweep_lens(sensor_t *s, char *note, size_t size) {
  if (!af_on) {
    snprintf(note, size, "no focus");
    return;
  }
  af_command(s, 0x08);  // 초점 MCU 를 풀어 둔다
  int best = 0;
  size_t best_len = 0;
  auto probe = [&](int pos) {
    if (pos < 0 || pos > 1023) return;
    set_lens(s, pos);
    size_t len = roi_size();
    if (len > best_len) best_len = len, best = pos;
  };
  for (int pos = 0; pos < 1024; pos += 128) probe(pos);  // 128 간격 8곳
  int center = best;
  for (int d : {-64, -32, 32, 64}) probe(center + d);  // 가장 선명한 곳 둘레
  set_lens(s, best);
  roi_size();
  snprintf(note, size, "lens %d, %uKB", best, (unsigned)(best_len / 1000));
}

bool camera_scan_zoom(int nx0, int ny0, int nx1, int ny1) {
  if (!running || video_mode || !full_hts) return false;
  sensor_t *s = esp_camera_sensor_get();
  uint32_t t0 = millis();
  int out_w = full_x1 - full_x0 + 1 - 2 * full_ox, out_h = full_y1 - full_y0 + 1 - 2 * full_oy;  // 2560x1920
  // 1단계 장 좌표 -> 센서 좌표. 좌우 반전으로 찍으면 센서 쪽은 거꾸로다
  if (mount_mirror) {
    int t = 1000 - nx0;
    nx0 = 1000 - nx1;
    nx1 = t;
  }
  int bx0 = nx0 * out_w / 1000, bx1 = nx1 * out_w / 1000, by0 = ny0 * out_h / 1000, by1 = ny1 * out_h / 1000;
  // 바코드보다 넉넉하게 (그 사이 손이 조금 움직여도), 640x480 ~ 1280x960
  int w = constrain((bx1 - bx0) * 16 / 10, 640, 1280) & ~31, h = constrain((by1 - by0) * 16 / 10, 480, 960) & ~15;
  int x = constrain((bx0 + bx1) / 2 - w / 2, 0, out_w - w), y = constrain((by0 + by1) / 2 - h / 2, 0, out_h - h);
  int win_w = w + 2 * full_ox, win_h = h + 2 * full_oy;
  int wx0 = full_x0 + x, wy0 = full_y0 + y;
  // 한 장의 줄 길이(HTS)·줄 수(VTS)도 창 크기만큼 줄여 장이 빨리 나오게 (전체 값 그대로면 초당 4장쯤)
  int hts = full_hts * win_w / (full_x1 - full_x0 + 1) + 64, vts = full_vts * win_h / (full_y1 - full_y0 + 1) + 32;
  roi_w = 0;
  scan_mode = true;  // 미리보기가 크기를 되돌리지 않게
  if (s->set_res_raw(s, wx0, wy0, wx0 + win_w - 1, wy0 + win_h - 1, full_ox, full_oy, hts, vts, w, h, false, false)) {
    camera_scan_end();
    return false;
  }
  camera_fb_t *fb = frame_of_width(w, 10, 1);
  if (!fb) {
    camera_scan_end();
    Serial.println("[camera] scan zoom failed");
    return false;
  }
  esp_camera_fb_return(fb);
  roi_w = w;
  char note[48];
  sweep_lens(s, note, sizeof(note));
  // 렌즈를 훑는 동안만 장을 빨리 받고, 그 뒤에는 사진 크기와 같은 느린 속도로 (센서 발열을 줄인다).
  // 스캔은 1초에 한두 장이면 된다
  s->set_reg(s, 0x380E, 0xff, full_vts >> 8);
  s->set_reg(s, 0x380F, 0xff, full_vts & 0xff);
  Serial.printf("[camera] scan zoom %dx%d at %d,%d (%lu ms, %s)\n", w, h, x, y, (unsigned long)(millis() - t0), note);
  return true;
}

bool camera_scan_grab(uint8_t screen_rot, uint8_t **jpg, size_t *len) {
  if (!running || !roi_w) return false;
  uint32_t t0 = millis();
  bool ok = take_frame(frame_of_width(roi_w, 3, 0), screen_rot, jpg, len);
  Serial.printf("[camera] scan grab %s (%u bytes, %lu ms)\n", ok ? "ok" : "failed", ok ? (unsigned)*len : 0,
                (unsigned long)(millis() - t0));
  return ok;
}

void camera_scan_end() {
  if (!running) return;
  sensor_t *s = esp_camera_sensor_get();
  bool was = scan_mode;
  scan_mode = false;
  roi_w = 0;
  s->set_framesize(s, preview_size);
  if (was && af_on) af_command(s, AF_CONTINUE_AUTO_FOCUS);
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
