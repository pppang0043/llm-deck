#pragma once

#include <Arduino.h>

// 찍은 사진(JPEG)을 저장한다: 보드의 microSD 카드(/photos)에 쓰고, PC 가 연결되어 있으면 PC 로도 보낸다.
// PC 로 보내는 형식:
//   먼저 알림 {"photo": {"name": "IMG_20261015_231500.jpg", "size": 123456}} (Wi-Fi 는 "url" 도 붙는다)
//   USB: USB_FRAME_PHOTO 프레임 [위치 4바이트][데이터]
//   블루투스: [0xFF][위치 3바이트][데이터] (소리 묶음과 길이가 다르게 자른다)
//   Wi-Fi: PC 가 알림의 url (http://기기/photo) 로 받아 간다
void photo_init();

// 사진을 저장·전송한다. jpg 는 이 함수가 맡아서 해제한다.
// 결과를 화면에 보여줄 짧은 글(한글)로 돌려준다.
// to_sd: SD 카드에도 저장할지 (진단용 화면 캡처는 저장하지 않는다)
const char *photo_save(uint8_t *jpg, size_t len, bool to_sd = true);

// SD 카드에 있는 사진(갤러리)을 이름 그대로 PC 로 다시 보낸다. jpg 는 이 함수가 맡아서 해제한다.
const char *photo_resend(const char *name, uint8_t *jpg, size_t len);

// Wi-Fi 로 받아 갈 수 있게 마지막 사진을 가지고 있는다
bool photo_last(const uint8_t **data, size_t *len, const char **name);

// PC 로 보내는 중인지 (블루투스는 수 초 걸린다)
bool photo_sending();
// 보내는 중이면 true 와 진행률(%)
bool photo_progress(int *pct);
// 지금까지 다 보낸(또는 실패한) 사진 수. 마지막 것의 성공 여부와 걸린 시간도 준다.
uint32_t photo_finished(bool *ok, uint32_t *ms, const char **name = nullptr);

// 진단용: 아무 파일이나 사진처럼 PC 로 보낸다 (USB·블루투스). data 는 ps_malloc 으로 잡은 것, 보낸 뒤 해제한다.
bool photo_send_file(const char *name, uint8_t *data, size_t len);

// 카드의 파일(갤러리의 동영상 등)을 이름 그대로 USB 로 연결한 PC 로 보낸다. 파일 전체를 메모리에 올리지 않고
// 16KB 씩 읽어 보내므로 크기 제한이 없고 화면도 멈추지 않는다. 형식은 사진과 같다 (알림 + USB_FRAME_PHOTO 조각).
// USB 가 아니거나 앞의 것을 보내는 중이라 줄에 못 넣으면 false.
bool photo_send_sd_file(const char *path);
// 카드 파일을 보내는 중인지 (path 를 주면 그 파일인지). 보내는 중인 파일은 지우거나 카드 연결을 끊지 않는다.
bool photo_streaming(const char *path = nullptr);
