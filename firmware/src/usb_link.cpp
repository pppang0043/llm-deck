#include "usb_link.h"

#include <string>

#include "link.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"

#define MAX_LINE 16384
// PC 에서 받던 것이 이만큼 끊기면 USB 를 다시 붙인다 (도우미는 USB 로 30초마다 보낸다)
#define RX_GAP_MS 120000

static std::string line;
static uint32_t rx_ms = 0;

void usb_link_init() {
  // Serial.begin() 보다 먼저 불러야 한다
  Serial.setRxBufferSize(8192);
  Serial.setTxBufferSize(4096);
  // PC 에서 아무도 읽지 않을 때 로그 출력 때문에 멈추지 않도록 한다
  Serial.setTxTimeoutMs(20);
}

void usb_reconnect(const char *why) {
  Serial.printf("[usb] reconnecting: %s\n", why);
  // D+ 풀업을 잠깐 떼면 PC 는 케이블을 뽑았다 꽂은 것으로 보고 처음부터 다시 연결한다.
  // 기기 재시작(esp_restart)으로는 USB 부분이 초기화되지 않아, PC 를 다시 켤 때 꼬인 USB 가 그대로 남은 적이 있다
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
  delay(200);
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
}

void usb_link_loop() {
  // PC 에서 받은 뒤로 아직 다시 붙이지 않았다. 다시 붙인 뒤에도 안 오면 되풀이하지 않는다 (도우미를 끈 PC, 충전기)
  static bool armed = false;
  if (Serial.available()) {
    rx_ms = millis() ? millis() : 1;
    armed = true;
  }
  if (armed && millis() - rx_ms > RX_GAP_MS) {
    armed = false;
    usb_reconnect("nothing from the PC for 2 minutes");
  }
  // 입력이 계속 쌓여도 link_loop/UI 가 돌아가야 큐가 비워지고 명령이 실행된다.
  // 긴 JSON 은 다음 loop 에 이어 받는다. 사진/터치 부하 중에도 입력만 읽으며 굶지 않는다.
  unsigned bytes = 0, lines = 0;
  while (Serial.available() && bytes < 2048 && lines < 4) {
    ++bytes;
    char c = Serial.read();
    if (c == '\n') {
      ++lines;
      if (!line.empty() && line[0] == '{') {
        String reply;
        if (!link_submit(LINK_USB, line.data(), line.size(), true, &reply))
          usb_send_event("{\"link_error\":\"not_queued\"}");
        else if (reply.length()) usb_send_event(reply.c_str());
      }
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

void usb_send_frame(uint8_t kind, const uint8_t *head, size_t head_len, const uint8_t *data, size_t len) {
  // 여러 태스크가 보내므로 프레임마다 버퍼를 따로 쓴다
  uint8_t frame[5 + 2048];
  size_t body = head_len + len;
  if (5 + body > sizeof(frame)) return;
  frame[0] = 0xA5;
  frame[1] = 0x5A;
  frame[2] = kind;
  frame[3] = body & 0xFF;
  frame[4] = body >> 8;
  if (head_len) memcpy(frame + 5, head, head_len);
  if (len) memcpy(frame + 5 + head_len, data, len);
  Serial.write(frame, 5 + body);
}

bool usb_host_present() {
  static uint32_t lost_ms = 0;
  if (Serial.isPlugged()) {
    lost_ms = 0;
    return true;
  }
  if (!lost_ms) lost_ms = millis() ? millis() : 1;
  return millis() - lost_ms < 3000;
}

uint32_t usb_last_rx_ms() {
  return rx_ms;
}
