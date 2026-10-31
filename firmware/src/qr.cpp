#include "qr.h"

#include <img_converters.h>
#include <jpeg_decoder.h>
#include <quirc.h>

// 이보다 큰 장은 풀지 않는다 (스캔 1단계 장은 1024x768)
#define QR_MAX_PIXELS (1280 * 960)

char *qr_decode_jpeg(const uint8_t *jpg, size_t len) {
  uint32_t t0 = millis();
  esp_jpeg_image_cfg_t cfg = {};
  cfg.indata = (uint8_t *)jpg;
  cfg.indata_size = len;
  esp_jpeg_image_output_t info = {};
  if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || !info.width || !info.height) return nullptr;
  int w = info.width, h = info.height;
  if (w * h > QR_MAX_PIXELS) return nullptr;

  // RGB565 로 푼 뒤 밝기(흑백)만 quirc 의 그림 자리에 옮기고, 푼 그림은 바로 버린다
  uint8_t *rgb = (uint8_t *)ps_malloc((size_t)w * h * 2);
  struct quirc *q = quirc_new();
  if (!rgb || !q || quirc_resize(q, w, h) < 0 || !jpg2rgb565(jpg, len, rgb, JPG_SCALE_NONE)) {
    free(rgb);
    if (q) quirc_destroy(q);
    return nullptr;
  }
  uint32_t t_jpeg = millis();
  uint8_t *img = quirc_begin(q, nullptr, nullptr);
  for (int i = 0; i < w * h; i++) {
    uint16_t c = rgb[2 * i] | (rgb[2 * i + 1] << 8);  // camera_preview 와 같은 바이트 순서
    int r = (c >> 11) << 3, g = ((c >> 5) & 0x3f) << 2, b = (c & 0x1f) << 3;
    img[i] = (r * 77 + g * 150 + b * 29) >> 8;
  }
  free(rgb);
  quirc_end(q);

  // 판독 중 큰 구조체(코드 4KB, 내용 9KB)는 스택 대신 PSRAM 에
  struct quirc_code *code = (struct quirc_code *)ps_malloc(sizeof(struct quirc_code));
  struct quirc_data *data = (struct quirc_data *)ps_malloc(sizeof(struct quirc_data));
  char *text = nullptr;
  int n = quirc_count(q);
  for (int i = 0; code && data && i < n && !text; i++) {
    quirc_extract(q, i, code);
    quirc_decode_error_t err = quirc_decode(code, data);
    if (err == QUIRC_ERROR_DATA_ECC) {  // 좌우가 뒤집혀 찍혔을 수 있다 (거울 상)
      quirc_flip(code);
      err = quirc_decode(code, data);
    }
    if (err || !data->payload_len) {
      if (err) Serial.printf("[qr] code %d: %s\n", i, quirc_strerror(err));
      continue;
    }
    text = (char *)ps_malloc(data->payload_len + 1);
    if (text) {
      memcpy(text, data->payload, data->payload_len);
      text[data->payload_len] = 0;
    }
  }
  free(code);
  free(data);
  quirc_destroy(q);
  Serial.printf("[qr] %dx%d, %d found, %s (%lu ms = jpeg %lu + quirc %lu)\n", w, h, n, text ? "read" : "none",
                (unsigned long)(millis() - t0), (unsigned long)(t_jpeg - t0), (unsigned long)(millis() - t_jpeg));
  return text;
}
