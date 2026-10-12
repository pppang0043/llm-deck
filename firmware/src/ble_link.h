#pragma once

#include <Arduino.h>

// 블루투스(BLE)로 PC 와 통신한다. "Nordic UART" 라는 널리 쓰이는 서비스 형식을 따른다.
// PC -> 기기: JSON 을 여러 조각으로 나눠 쓰고 줄바꿈으로 끝낸다.
// 기기 -> PC: 마이크 소리. BLE 는 느려서 ADPCM 으로 1/4 로 압축해 보낸다.
//   [순번 2바이트][ADPCM 시작값 2바이트][ADPCM 단계 1바이트][샘플당 4비트 데이터]
#define BLE_NAME "LLM-Deck"

void ble_link_init();
void ble_send_audio(uint32_t seq, const int16_t *pcm, size_t samples);
