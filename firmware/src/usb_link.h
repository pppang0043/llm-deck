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
// PC 의 USB 에 꽂혀 있는지. USB 신호가 아주 잠깐 끊겼다 이어지는 일이 있어, 3초 넘게 계속 끊겨야 false.
bool usb_host_present();
// PC 가 USB 로 마지막에 무언가 보낸 때 (millis). 보낸 적 없으면 0.
// USB 신호(usb_host_present)가 PC 잠자기 등으로 몇 분씩 끊긴 것처럼 보이는 일이 있어, 배터리가 전원을 판단할 때 함께 본다.
uint32_t usb_last_rx_ms();
// 케이블을 뽑았다 꽂은 것처럼 USB 를 다시 붙인다. PC 를 다시 켠 뒤 USB 가 꼬여 PC 가 보낸 것이 오지 않을 때.
// usb_link_loop 가 PC 에서 2분 넘게 아무것도 오지 않으면 스스로 부른다
void usb_reconnect(const char *why);
void usb_send_audio(uint32_t seq, const int16_t *pcm, size_t samples);
void usb_send_event(const char *json);
void usb_send_photo_chunk(uint32_t offset, const uint8_t *data, size_t len);
// 묶음 하나를 보낸다: 내용 = head + data (합쳐서 2KB 이하)
void usb_send_frame(uint8_t kind, const uint8_t *head, size_t head_len, const uint8_t *data, size_t len);
