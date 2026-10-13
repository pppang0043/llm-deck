#include "display.h"

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <lvgl.h>

#include "pins.h"

#define CST816_ADDR 0x15
#define CST816_REG_ID 0xA7
#define CST816_REG_TOUCH 0x02

// 한 번에 그리는 줄 수. 클수록 빠르지만 메모리를 더 쓴다.
#define DRAW_BUF_LINES 40

static Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, LCD_MISO);
static Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0 /* rotation */, true /* IPS */, LCD_W, LCD_H);

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;
static uint8_t rotation = 0;

static bool touch_read_reg(uint8_t reg, uint8_t *data, uint8_t len) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)CST816_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) data[i] = Wire.read();
  return true;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
  lv_disp_flush_ready(drv);
}

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  // [0]=손가락 수, [1..2]=X, [3..4]=Y
  uint8_t raw[5];
  if (touch_read_reg(CST816_REG_TOUCH, raw, sizeof(raw)) && raw[0] > 0) {
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
  uint8_t id = 0;
  if (touch_read_reg(CST816_REG_ID, &id, 1)) {
    Serial.printf("[display] touch chip id 0x%02X\n", id);
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
