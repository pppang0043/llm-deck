#pragma once

#include <Arduino.h>

// USB 케이블(시리얼)로 PC 와 통신한다.
// PC -> 기기: 한 줄에 JSON 하나.
// 기기 -> PC: 로그 글자 사이에 아래 형식의 묶음(프레임)을 끼워 보낸다.
//   [0xA5 0x5A][종류 1바이트][길이 2바이트][내용]
#define USB_FRAME_AUDIO 1
#define USB_FRAME_EVENT 2  // 내용은 JSON 글자 (link.h 의 link_send_event)
#define USB_FRAME_PHOTO 3  // 사진 조각: [위치 4바이트][JPEG 데이터] (photo.h)
#define USB_FRAME_VIDEO 4  // 동영상 조각 (video.h)

void usb_link_init();
void usb_link_loop();
void usb_send_audio(uint32_t seq, const int16_t *pcm, size_t samples);
void usb_send_event(const char *json);
void usb_send_photo_chunk(uint32_t offset, const uint8_t *data, size_t len);
// 묶음 하나를 보낸다: 내용 = head + data (합쳐서 2KB 이하)
void usb_send_frame(uint8_t kind, const uint8_t *head, size_t head_len, const uint8_t *data, size_t len);
