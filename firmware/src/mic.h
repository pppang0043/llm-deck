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

// 질문을 녹음하는 동안만 켠다 (위 켜기/끄기와 따로 둔다. 둘 중 하나라도 켜져 있으면 보낸다).
// PC 는 이 소리를 질문 녹음에 먼저 쓰고, 조용하면 PC 마이크로 녹음한다 (companion/assistant.py)
void mic_set_ask(bool on);
// 마이크가 달려 있는지. 켤 때 잠깐 들어 보고, 보내는 동안에도 고친다.
// 마이크가 없으면 I2S 가 늘 같은 값(0 이나 -1)만 읽는다
bool mic_present();

// 최근 입력 크기 0~100 (화면 레벨 미터용)
uint8_t mic_level();

// 오디오를 보낼 PC 주소
void mic_set_target(IPAddress ip);
IPAddress mic_target();
