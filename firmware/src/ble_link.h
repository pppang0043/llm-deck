#pragma once

#include <Arduino.h>

// 블루투스(BLE)로 PC 와 통신한다. "Nordic UART" 라는 널리 쓰이는 서비스 형식을 따른다.
// PC -> 기기: JSON 을 여러 조각으로 나눠 쓰고 줄바꿈으로 끝낸다.
// 기기 -> PC: 마이크 소리. BLE 는 느려서 ADPCM 으로 1/4 로 압축해 보낸다.
//   [순번 2바이트][ADPCM 시작값 2바이트][ADPCM 단계 1바이트][샘플당 4비트 데이터]
// 기기 -> PC: 알림(JSON 글자, '{' 로 시작). 소리 묶음과 길이가 달라서 구별된다.
// 기기 -> PC: 사진 조각 [0xFF][위치 3바이트][JPEG 데이터]. 소리 묶음과 길이가 겹치지 않게 자른다.
#define BLE_NAME "LLM-Deck"

void ble_link_init();
bool ble_link_connected();
// 끄면 연결을 끊고 광고(다른 기기가 찾을 수 있게 알리기)를 멈춘다
void ble_link_set_enabled(bool on);
void ble_send_audio(uint32_t seq, const int16_t *pcm, size_t samples);
void ble_send_event(const char *json);
// 한 번에 보낼 수 있는 사진 데이터 크기 (연결이 없으면 0)
size_t ble_photo_chunk_max();
// 보내기 버퍼가 비기를 기다리며 보낸다. 연결이 끊기면 false.
bool ble_send_photo_chunk(uint32_t offset, const uint8_t *data, size_t len);
