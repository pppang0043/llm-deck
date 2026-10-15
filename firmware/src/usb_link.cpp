#include "usb_link.h"

#include <string>

#include "link.h"

#define MAX_LINE 16384

static std::string line;

void usb_link_init() {
  // Serial.begin() 보다 먼저 불러야 한다
  Serial.setRxBufferSize(8192);
  Serial.setTxBufferSize(4096);
  // PC 에서 아무도 읽지 않을 때 로그 출력 때문에 멈추지 않도록 한다
  Serial.setTxTimeoutMs(20);
}

void usb_link_loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (!line.empty() && line[0] == '{') link_submit(LINK_USB, line.data(), line.size());
      line.clear();
    } else if (line.size() < MAX_LINE) {
      line += c;
    }
  }
}

void usb_send_audio(uint32_t seq, const int16_t *pcm, size_t samples) {
  // 로그 출력과 섞이지 않도록 프레임 전체를 한 번에 쓴다
  static uint8_t frame[5 + 4 + 320 * 2];
  size_t body = 4 + samples * 2;
  if (5 + body > sizeof(frame)) return;
  frame[0] = 0xA5;
  frame[1] = 0x5A;
  frame[2] = USB_FRAME_AUDIO;
  frame[3] = body & 0xFF;
  frame[4] = body >> 8;
  memcpy(frame + 5, &seq, 4);
  memcpy(frame + 9, pcm, samples * 2);
  Serial.write(frame, 5 + body);
}

void usb_send_event(const char *json) {
  uint8_t frame[5 + 200];
  size_t body = strlen(json);
  if (5 + body > sizeof(frame)) return;
  frame[0] = 0xA5;
  frame[1] = 0x5A;
  frame[2] = USB_FRAME_EVENT;
  frame[3] = body & 0xFF;
  frame[4] = body >> 8;
  memcpy(frame + 5, json, body);
  Serial.write(frame, 5 + body);
}

void usb_send_photo_chunk(uint32_t offset, const uint8_t *data, size_t len) {
  static uint8_t frame[5 + 4 + 1024];
  size_t body = 4 + len;
  if (5 + body > sizeof(frame)) return;
  frame[0] = 0xA5;
  frame[1] = 0x5A;
  frame[2] = USB_FRAME_PHOTO;
  frame[3] = body & 0xFF;
  frame[4] = body >> 8;
  memcpy(frame + 5, &offset, 4);
  memcpy(frame + 9, data, len);
  Serial.write(frame, 5 + body);
}
