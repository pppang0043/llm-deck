#include "display.h"

#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <Wire.h>
#include <lvgl.h>

#include "pins.h"

#define CST816_ADDR 0x15
#define CST816_REG_ID 0xA7
#define CST816_REG_TOUCH 0x02
// 0 이 아니면 자동 절전을 끈다. 켜 두면 2초쯤 안 만졌을 때 잠들어서 다음 첫 터치를 놓치거나 늦게 알린다.
#define CST816_REG_DIS_AUTO_SLEEP 0xFE

// 한 번에 그리는 줄 수. 클수록 빠르지만 메모리를 더 쓴다.
#define DRAW_BUF_LINES 40

// SD 카드와 같은 SPI 선(HSPI)을 함께 쓴다. 그리기는 SPIClass 트랜잭션 잠금을 잡아 SD 쓰기와 겹치지 않게 한다.
static SPIClass spi(HSPI);
static Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, LCD_MISO, HSPI, true);
static Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0 /* rotation */, true /* IPS */, LCD_W, LCD_H);

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_disp_t *main_disp;
// 미러링 중 기기 화면: PC 창은 원래 화면(기본 디스플레이)을 계속 받고, 기기 LCD 에는 이 디스플레이를 그린다
static lv_disp_draw_buf_t device_draw_buf;
static lv_disp_drv_t device_drv;
static lv_disp_t *device_disp;
static bool split = false;
static lv_indev_drv_t indev_drv;
static lv_indev_drv_t device_indev_drv;  // 나눠 쓰는 동안 기기 화면을 만진 것
// 지난번에 읽은 손가락 (PC 에서 누른 것은 빼고)
static bool finger_down = false;
static lv_point_t finger_at;
static uint8_t rotation = 0;
static volatile uint32_t touched_ms = 0;
// 화면이 꺼져(어두워져) 있을 때의 첫 터치는 깨우기만 하고 버튼으로 넘기지 않는다
static volatile bool sleeping = false;
static bool swallow = false;
static bool device_swallow = false;

// 미러링: 화면에 보낸 그림을 PSRAM 에 똑같이 모아 둔다 (켜 둔 동안만. 다시 그리지 않으므로 화면이 느려지지 않는다)
static uint16_t *shadow = nullptr;
static bool shadow_dirty = false;
// PC 에서 누른 것 (미러링 창). 손가락처럼 터치 읽기 때마다 하나씩 꺼내 LVGL 에 알린다.
struct RemoteTouch {
  int16_t x, y;
  bool down;
};
#define REMOTE_QUEUE 16
static RemoteTouch remote_queue[REMOTE_QUEUE];
static uint8_t remote_head = 0, remote_tail = 0;
static RemoteTouch remote_now;
static bool remote_active = false;  // 누르는 중이거나, 뗀 것을 아직 LVGL 에 알리지 않았다

static bool touch_read_reg(uint8_t reg, uint8_t *data, uint8_t len) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)CST816_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) data[i] = Wire.read();
  return true;
}

static bool touch_write_reg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

static void draw_lcd(const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  // SD 카드와 같은 SPI 선을 쓴다. 동영상을 카드에 쓰는 태스크와 겹치지 않게, SD 라이브러리가 쓰는 것과
  // 같은 잠금(SPIClass 의 트랜잭션)을 잡고 그린다. 화면 드라이버는 그리기 전에 자기 속도로 다시 맞춘다.
  SPIClass &bus = display_spi();
  bus.beginTransaction(SPISettings(80000000, MSBFIRST, SPI_MODE0));
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
  bus.endTransaction();
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  if (!split) draw_lcd(area, color_p);
  if (shadow) {
    int dw = display_width();
    for (uint32_t row = 0; row < h; row++) {
      memcpy(shadow + (area->y1 + row) * dw + area->x1, (uint16_t *)color_p + row * w, w * 2);
    }
    shadow_dirty = true;
  }
  lv_disp_flush_ready(drv);
}

static void device_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  if (split) draw_lcd(area, color_p);
  lv_disp_flush_ready(drv);
}

// I2C 선 막힘 풀기. 터치 칩, 기울기 센서, 스피커 모듈이 한 선을 쓰는데, 어느 기기가 데이터 선(SDA)을
// 낮게 잡은 채 멈추면 모두 대답이 없어진다. 시계 선(SCL)을 직접 9번 흔들어 놓아주게 한 뒤 다시 연다.
#define IMU_ADDR 0x6B
#define TOUCH_FAIL_LIMIT 30  // 터치를 이만큼 연달아 못 읽으면 (약 1초) 선을 살펴본다

static bool bus_alive() {
  // 기울기 센서는 늘 대답하므로, 이것도 대답이 없으면 선이 막힌 것으로 본다
  Wire.beginTransmission(IMU_ADDR);
  return Wire.endTransmission(true) == 0;
}

static void recover_bus() {
  Wire.end();
  pinMode(TP_SDA, INPUT_PULLUP);
  pinMode(TP_SCL, OUTPUT_OPEN_DRAIN);
  for (int i = 0; i < 9 && digitalRead(TP_SDA) == LOW; i++) {
    digitalWrite(TP_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(TP_SCL, HIGH);
    delayMicroseconds(5);
  }
  // 멈춤(STOP) 신호: SCL 이 높을 때 SDA 를 낮음에서 높음으로
  pinMode(TP_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(TP_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(TP_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(TP_SDA, HIGH);
  delayMicroseconds(5);
  Wire.begin(TP_SDA, TP_SCL);
  Wire.setClock(400000);
  // 터치 칩이 다시 켜졌을 수도 있으니 자동 절전을 다시 끈다
  touch_write_reg(CST816_REG_DIS_AUTO_SLEEP, 1);
}

static void watch_bus(bool ok) {
  static int fails = 0;
  static uint32_t checked_ms = 0;
  if (ok) {
    fails = 0;
    return;
  }
  if (++fails < TOUCH_FAIL_LIMIT || millis() - checked_ms < 5000) return;
  fails = 0;
  checked_ms = millis();
  if (bus_alive()) return;  // 터치 칩만 잠깐 대답을 안 한 것
  Serial.println("[i2c] bus stuck, recovering");
  recover_bus();
  Serial.printf("[i2c] %s\n", bus_alive() ? "recovered" : "still stuck");
}

void display_i2c_test() {
  Serial.println("[i2c] test: holding SDA low");
  pinMode(TP_SDA, OUTPUT);
  digitalWrite(TP_SDA, LOW);
}

// 터치 칩은 항상 세로(돌리지 않은) 기준 좌표를 주므로, 화면을 돌린 만큼 바꿔 준다
static void finger_point(const uint8_t *raw, lv_point_t *pt) {
  int px = ((raw[1] & 0x0F) << 8) | raw[2];
  int py = ((raw[3] & 0x0F) << 8) | raw[4];
  switch (rotation) {
    case 1:
      pt->x = py;
      pt->y = LCD_W - 1 - px;
      break;
    case 2:
      pt->x = LCD_W - 1 - px;
      pt->y = LCD_H - 1 - py;
      break;
    case 3:
      pt->x = LCD_H - 1 - py;
      pt->y = px;
      break;
    default:
      pt->x = px;
      pt->y = py;
      break;
  }
}

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  // [0]=손가락 수, [1..2]=X, [3..4]=Y
  uint8_t raw[5];
  bool read_ok = touch_read_reg(CST816_REG_TOUCH, raw, sizeof(raw));
  watch_bus(read_ok);
  finger_down = read_ok && raw[0] > 0;
  if (finger_down) {
    finger_point(raw, &finger_at);
    touched_ms = millis();
  }
  // 나눠 쓰는 동안 손가락은 기기 화면(device_touch_cb)으로 가고, 원래 화면은 PC 에서 누른 것만 받는다
  bool down = finger_down && !split;
  // 손가락이 없으면 PC 에서 누른 것을 쓴다 (좌표는 이미 지금 화면 방향 기준)
  bool remote = false;
  if (!down) {
    if (!remote_active && remote_head != remote_tail) {
      remote_now = remote_queue[remote_tail];
      remote_tail = (remote_tail + 1) % REMOTE_QUEUE;
      remote_active = true;
    } else if (remote_active && !remote_now.down) {
      remote_active = false;  // 뗀 것을 지난번에 알렸다
      if (remote_head != remote_tail) {
        remote_now = remote_queue[remote_tail];
        remote_tail = (remote_tail + 1) % REMOTE_QUEUE;
        remote_active = true;
      }
    } else if (remote_active && remote_head != remote_tail) {
      // 누르는 중에 다음 것(끌기, 떼기)이 왔다
      remote_now = remote_queue[remote_tail];
      remote_tail = (remote_tail + 1) % REMOTE_QUEUE;
    }
    remote = remote_active;
    down = remote && remote_now.down;
  }
  if (down) touched_ms = millis();
  if (down && sleeping) swallow = true;
  if (swallow) {
    // 손을 뗄 때까지 눌리지 않은 것으로 알린다
    if (!down) swallow = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (remote) {
    data->point.x = remote_now.x;
    data->point.y = remote_now.y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  } else if (down) {
    data->point = finger_at;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// 나눠 쓰는 동안의 기기 화면 터치. 칩은 touch_cb 가 읽고 (같은 주기로 먼저 돈다), 여기서는 그 값을 쓴다.
static void device_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  bool down = split && finger_down;
  if (down && sleeping) device_swallow = true;
  if (device_swallow) {
    if (!down) device_swallow = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  data->point = finger_at;
  data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void display_init() {
  if (!gfx->begin()) Serial.println("[display] gfx begin failed");
  gfx->fillScreen(0x0000);

  // 백라이트 밝기 80%
  ledcAttach(LCD_BL, 5000, 10);
  ledcWrite(LCD_BL, 1023 * 80 / 100);

  Wire.begin(TP_SDA, TP_SCL);
  // 터치를 10ms 마다 읽으므로 선을 빠르게 (같은 선의 기울기 센서, 스피커 모듈도 400kHz 를 지원한다)
  Wire.setClock(400000);
  uint8_t id = 0;
  if (touch_read_reg(CST816_REG_ID, &id, 1)) {
    Serial.printf("[display] touch chip id 0x%02X\n", id);
    if (!touch_write_reg(CST816_REG_DIS_AUTO_SLEEP, 1)) Serial.println("[display] touch auto sleep not disabled");
  } else {
    Serial.println("[display] touch chip not found");
  }

  lv_init();

  size_t buf_px = LCD_W * DRAW_BUF_LINES;
  lv_color_t *buf = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, buf_px);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = LCD_W;
  disp_drv.ver_res = LCD_H;
  disp_drv.flush_cb = flush_cb;
  disp_drv.draw_buf = &draw_buf;
  main_disp = lv_disp_drv_register(&disp_drv);

  // 그리기는 한 디스플레이씩 차례로 하고 flush_cb 안에서 끝나므로 그리기 버퍼를 같이 쓴다
  lv_disp_draw_buf_init(&device_draw_buf, buf, NULL, buf_px);
  lv_disp_drv_init(&device_drv);
  device_drv.hor_res = LCD_W;
  device_drv.ver_res = LCD_H;
  device_drv.flush_cb = device_flush_cb;
  device_drv.draw_buf = &device_draw_buf;
  device_disp = lv_disp_drv_register(&device_drv);  // 기본 디스플레이는 먼저 만든 main_disp 그대로

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_cb;
  indev_drv.disp = main_disp;
  // 손을 뗀 뒤 미끄러지는 힘이 한 번에 줄어드는 비율(%). 기본 10 은 사용량 페이지를 넘기려면 화면 절반 넘게 끌거나
  // 세게 튕겨야 했다. 줄이면 가볍게 튕겨도 다음 페이지로 넘어가고 목록도 더 멀리 미끄러진다.
  // (lv_conf.h 의 LV_INDEV_DEF_* 는 lv_hal_indev.h 가 다시 정의해 버려 여기서 정한다)
  indev_drv.scroll_throw = 5;
  lv_indev_drv_register(&indev_drv);

  lv_indev_drv_init(&device_indev_drv);
  device_indev_drv.type = LV_INDEV_TYPE_POINTER;
  device_indev_drv.read_cb = device_touch_cb;
  device_indev_drv.disp = device_disp;
  lv_indev_drv_register(&device_indev_drv);
}

void display_set_rotation(uint8_t value) {
  rotation = value & 3;
  gfx->setRotation(rotation);
  disp_drv.hor_res = display_width();
  disp_drv.ver_res = display_height();
  lv_disp_drv_update(main_disp, &disp_drv);
  device_drv.hor_res = display_width();
  device_drv.ver_res = display_height();
  lv_disp_drv_update(device_disp, &device_drv);
}

uint8_t display_rotation() {
  return rotation;
}

int display_width() {
  return (rotation & 1) ? LCD_H : LCD_W;
}

int display_height() {
  return (rotation & 1) ? LCD_W : LCD_H;
}

void display_loop() {
  lv_timer_handler();
}

SPIClass &display_spi() {
  // 화면(gfx)이 먼저 SPI 를 잡은 뒤에 열어야 한다. 화면보다 먼저 열면 부팅 중에 멈춘다.
  static bool started = false;
  if (!started) {
    spi.begin(LCD_SCLK, LCD_MISO, LCD_MOSI, -1);
    started = true;
  }
  return spi;
}

void display_set_brightness(uint8_t pct) {
  // 0 은 절전으로 화면을 끈 상태. 그 밖에는 너무 어두워 안 보이지 않게 10% 이상
  if (pct > 0) pct = constrain(pct, 10, 100);
  ledcWrite(LCD_BL, 1023 * pct / 100);
}

uint32_t display_last_touch_ms() {
  return touched_ms;
}

void display_set_sleeping(bool on) {
  sleeping = on;
}

void display_poke() {
  touched_ms = millis();
}

void display_mirror(bool on) {
  if (on == (shadow != nullptr)) return;
  if (!on) {
    free(shadow);
    shadow = nullptr;
    return;
  }
  shadow = (uint16_t *)ps_malloc(LCD_W * LCD_H * 2);
  if (!shadow) return;
  memset(shadow, 0, LCD_W * LCD_H * 2);
  // 처음 한 장을 채우려고 화면 전체를 다시 그린다
  lv_obj_invalidate(lv_scr_act());
}

bool display_mirror_take(uint16_t *out, int *w, int *h) {
  if (!shadow || !shadow_dirty) return false;
  *w = display_width();
  *h = display_height();
  memcpy(out, shadow, *w * *h * 2);
  shadow_dirty = false;
  return true;
}

void display_remote_touch(int x, int y, bool down) {
  uint8_t next = (remote_head + 1) % REMOTE_QUEUE;
  if (next == remote_tail) return;  // 넘치면 버린다
  x = constrain(x, 0, display_width() - 1);
  y = constrain(y, 0, display_height() - 1);
  remote_queue[remote_head] = {(int16_t)x, (int16_t)y, down};
  remote_head = next;
}

void display_split(bool on) {
  if (on == split) return;
  split = on;
  // 누르고 있던 손가락은 뗄 때까지 어느 쪽도 누르지 않는다 (바뀐 화면의 버튼이 눌리지 않게)
  if (finger_down) swallow = device_swallow = true;
  // 기기 LCD 에 새로 보일 쪽을 처음부터 다시 그린다
  lv_obj_invalidate(lv_disp_get_scr_act(on ? device_disp : main_disp));
}

bool display_device_pressed() {
  return split && finger_down;
}

bool display_split_on() {
  return split;
}

lv_obj_t *display_device_screen() {
  return lv_disp_get_scr_act(device_disp);
}
