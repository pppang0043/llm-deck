#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// 오디오 형식: 16kHz, 16bit, 모노. 20ms(320샘플)마다 UDP 패킷 하나.
#define MIC_SAMPLE_RATE 16000
#define MIC_FRAME_SAMPLES 320
#define MIC_UDP_PORT 4010

void mic_init();
void mic_set_on(bool on);
bool mic_is_on();

// 최근 입력 크기 0~100 (화면 레벨 미터용)
uint8_t mic_level();

// 오디오를 보낼 PC 주소
void mic_set_target(IPAddress ip);
IPAddress mic_target();
