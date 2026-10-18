#include "mirror.h"

#include <img_converters.h>

#include "display.h"
#include "link.h"
#include "usb_link.h"

// 창이 이만큼 아무 말이 없으면 (닫혔거나 도우미가 꺼짐) 끈다
#define KEEP_MS 6000
// 장 사이의 최소 간격. 압축은 다른 코어에서 하지만 화면 복사와 USB 쓰기가 화면 반응을 늦추지 않게 둔다.
#define MIN_GAP_MS 140
#define JPEG_QUALITY 70
#define CHUNK 2000

struct __attribute__((packed)) Head {
  uint32_t no, offset, total;
  uint16_t w, h;
};

static uint16_t *grab = nullptr;  // 보낼 화면 (PSRAM)
static int grab_w, grab_h;
static volatile bool busy = false;  // 압축·보내는 중
static uint32_t until_ms = 0;
static uint32_t last_ms = 0;
static uint32_t frame_no = 0;
static TaskHandle_t task = nullptr;

static void send_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int n = grab_w * grab_h;
    // LVGL 색은 작은 끝(little-endian), JPEG 변환은 큰 끝을 받는다
    for (int i = 0; i < n; i++) grab[i] = (grab[i] << 8) | (grab[i] >> 8);
    uint8_t *jpg = nullptr;
    size_t len = 0;
    if (fmt2jpg((uint8_t *)grab, n * 2, grab_w, grab_h, PIXFORMAT_RGB565, JPEG_QUALITY, &jpg, &len)) {
      Head head = {frame_no++, 0, (uint32_t)len, (uint16_t)grab_w, (uint16_t)grab_h};
      for (size_t off = 0; off < len; off += CHUNK) {
        head.offset = off;
        usb_send_frame(USB_FRAME_MIRROR, (const uint8_t *)&head, sizeof(head), jpg + off, min((size_t)CHUNK, len - off));
      }
      free(jpg);
    }
    busy = false;
  }
}

void mirror_init() {
  grab = (uint16_t *)ps_malloc(320 * 240 * 2);
  if (grab) xTaskCreatePinnedToCore(send_task, "mirror", 8192, nullptr, 1, &task, 0);
}

void mirror_keep(bool on) {
  if (!task) return;
  if (on && !until_ms) Serial.println("[mirror] on");
  if (!on && until_ms) Serial.println("[mirror] off");
  until_ms = on ? millis() + KEEP_MS : 0;
  display_mirror(on);
}

void mirror_loop() {
  if (!until_ms) return;
  if ((int32_t)(millis() - until_ms) > 0 || link_active() != LINK_USB) {
    mirror_keep(false);
    return;
  }
  if (busy || millis() - last_ms < MIN_GAP_MS) return;
  if (!display_mirror_take(grab, &grab_w, &grab_h)) return;
  last_ms = millis();
  busy = true;
  xTaskNotifyGive(task);
}
