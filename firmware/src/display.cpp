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
static lv_indev_drv_t indev_drv;
static uint8_t rotation = 0;
static volatile uint32_t touched_ms = 0;
// 화면이 꺼져(어두워져) 있을 때의 첫 터치는 깨우기만 하고 버튼으로 넘기지 않는다
static volatile bool sleeping = false;
static bool swallow = false;

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

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  // SD 카드와 같은 SPI 선을 쓴다. 동영상을 카드에 쓰는 태스크와 겹치지 않게, SD 라이브러리가 쓰는 것과
  // 같은 잠금(SPIClass 의 트랜잭션)을 잡고 그린다. 화면 드라이버는 그리기 전에 자기 속도로 다시 맞춘다.
  SPIClass &bus = display_spi();
  bus.beginTransaction(SPISettings(80000000, MSBFIRST, SPI_MODE0));
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
  bus.endTransaction();
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

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  // [0]=손가락 수, [1..2]=X, [3..4]=Y
  uint8_t raw[5];
  bool read_ok = touch_read_reg(CST816_REG_TOUCH, raw, sizeof(raw));
  watch_bus(read_ok);
  bool down = read_ok && raw[0] > 0;
  if (down) touched_ms = millis();
  if (down && sleeping) swallow = true;
  if (swallow) {
    // 손을 뗄 때까지 눌리지 않은 것으로 알린다
    if (!down) swallow = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (down) {
    // 터치 칩은 항상 세로(돌리지 않은) 기준 좌표를 주므로, 화면을 돌린 만큼 바꿔 준다
    int px = ((raw[1] & 0x0F) << 8) | raw[2];
    int py = ((raw[3] & 0x0F) << 8) | raw[4];
    switch (rotation) {
      case 1:
        data->point.x = py;
        data->point.y = LCD_W - 1 - px;
        break;
      case 2:
        data->point.x = LCD_W - 1 - px;
        data->point.y = LCD_H - 1 - py;
        break;
      case 3:
        data->point.x = LCD_H - 1 - py;
        data->point.y = px;
        break;
      default:
        data->point.x = px;
        data->point.y = py;
        break;
    }
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
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
  lv_disp_drv_register(&disp_drv);

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_cb;
  lv_indev_drv_register(&indev_drv);
}

void display_set_rotation(uint8_t value) {
  rotation = value & 3;
  gfx->setRotation(rotation);
  disp_drv.hor_res = display_width();
  disp_drv.ver_res = display_height();
  lv_disp_drv_update(lv_disp_get_default(), &disp_drv);
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
