#pragma once

#include <Arduino.h>

// USB 케이블(시리얼)로 PC 와 통신한다.
// PC -> 기기: 한 줄에 JSON 하나.
// 기기 -> PC: 로그 글자 사이에 아래 형식의 묶음(프레임)을 끼워 보낸다.
//   [0xA5 0x5A][종류 1바이트][길이 2바이트][내용]
#define USB_FRAME_AUDIO 1
#define USB_FRAME_EVENT 2  // 내용은 JSON 글자 (link.h 의 link_send_event)

void usb_link_init();
void usb_link_loop();
void usb_send_audio(uint32_t seq, const int16_t *pcm, size_t samples);
void usb_send_event(const char *json);
