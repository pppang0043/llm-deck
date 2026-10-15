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
const char *photo_save(uint8_t *jpg, size_t len);

// Wi-Fi 로 받아 갈 수 있게 마지막 사진을 가지고 있는다
bool photo_last(const uint8_t **data, size_t *len, const char **name);

// PC 로 보내는 중인지 (블루투스는 수 초 걸린다)
bool photo_sending();
