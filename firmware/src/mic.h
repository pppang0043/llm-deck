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
// 마이크 선 진단 (설정 > 알림 및 사운드). 시작하면 마이크 작업이 약 2초 검사하고, 끝나면 mic_diag_result() 가 글을 준다.
// 검사 중이면 nullptr. 값이 움직이는지 다시 보므로 마이크를 단 뒤 재부팅 없이 mic_present() 도 고쳐진다
void mic_diag_start();
const char *mic_diag_result();

// 최근 입력 크기 0~100 (화면 레벨 미터용)
uint8_t mic_level();

// 오디오를 보낼 PC 주소
void mic_set_target(IPAddress ip);
IPAddress mic_target();
