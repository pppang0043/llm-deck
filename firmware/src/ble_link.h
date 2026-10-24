#pragma once

#include <Arduino.h>

// 블루투스(BLE)로 PC 와 통신한다. "Nordic UART" 라는 널리 쓰이는 서비스 형식을 따른다.
// PC -> 기기: JSON 을 여러 조각으로 나눠 쓰고 줄바꿈으로 끝낸다.
// 기기 -> PC: 마이크 소리. BLE 는 느려서 ADPCM 으로 1/4 로 압축해 보낸다.
//   [순번 2바이트][ADPCM 시작값 2바이트][ADPCM 단계 1바이트][샘플당 4비트 데이터]
// 기기 -> PC: 알림(JSON 글자, '{' 로 시작). 소리 묶음과 길이가 달라서 구별된다.
// 기기 -> PC: 사진 조각 [0xFF][위치 3바이트][JPEG 데이터]. 소리 묶음과 길이가 겹치지 않게 자른다.
//
// 인증. BLE 는 암호화 없이 붙으므로, 근처의 누구나 기기에 명령을 보내거나 기기인 척 PC 에 입력·앱 실행을
// 시킬 수 없게 Wi-Fi 와 같은 키(USB 로 한 번 받아 간 것, net.h)로 서명한다. 서명은 앞 16바이트만 쓴다.
//   1. PC 가 붙으면 {"ble_hello":"<PC 도전값 32자>"} 를 보낸다
//   2. 기기는 그 PC 에만 {"ble_auth":{"id":"<기기 이름표>","n":"<기기 nonce 32자>","t":"<서명 32자>"}} 로 답한다
//        서명 = HMAC(키, "ble-dev" + PC 도전값 + 기기 nonce). PC 는 이것으로 진짜 기기인지 확인한다
//        (키가 없는 기기는 {"ble_auth":{"err":"unpaired"}})
//   3. PC -> 기기 줄: #<순번>.<서명 32자>.<JSON>   서명 = HMAC(키, "ble" + 기기 nonce + 순번 4바이트(LE) + JSON)
//   4. 기기 -> PC 알림: <JSON>|<순번>.<서명 32자>   서명 = HMAC(키, "ble-evt" + PC 도전값 + 순번 4바이트(LE) + JSON)
//   순번은 연결(도전값)마다 1 부터 늘고, 같거나 작은 것은 버린다. 서명이 맞는 줄을 보낸 PC 만 "사용 중"이 될 수 있어
//   마이크 소리·사진·알림은 인증된 PC 로만 간다. 20초 안에 인증하지 못한 연결은 끊는다 (자리 차지 방지).
#define BLE_NAME "LLM-Deck"

void ble_link_init();
// 메인 루프에서 부른다 (인증 못 한 연결 끊기, 거절 알림)
void ble_link_loop();
// 사용 중인 PC 가 있는지
bool ble_link_connected();
// 연결 세대까지 구분하는 사용 중인 PC 의 도전값 식별자 (끊겼으면 0).
uint64_t ble_primary_cookie();
// 끄면 연결을 끊고 광고(다른 기기가 찾을 수 있게 알리기)를 멈춘다
void ble_link_set_enabled(bool on);
void ble_send_audio(uint32_t seq, const int16_t *pcm, size_t samples);
// slot 은 해당 PC 에 직접 답할 때만 지정한다. 기본은 사용 중인 PC.
void ble_send_event(const char *json, int slot = -1);
// 한 번에 보낼 수 있는 사진 데이터 크기 (연결이 없으면 0)
size_t ble_photo_chunk_max();
// 보내기 버퍼가 비기를 기다리며 보낸다. 연결이 끊기면 false.
bool ble_send_photo_chunk(uint32_t offset, const uint8_t *data, size_t len);
bool ble_link_enabled();

// 여러 PC 동시 연결. slot 은 0 ~ BLE_MAX_PEERS-1.
#define BLE_MAX_PEERS 3
// 그 자리의 상태: 0 비어 있음, 1 기다리는 중, 2 사용 중. name 에 PC 이름(모르면 주소)을 넣는다.
int ble_peer_get(int slot, char *name, size_t size);
int ble_peer_count();
// 이 PC 를 사용 중으로 바꾼다 (마이크·사진·알림이 이 PC 로 간다). 다음에 다시 붙어도 이 PC 를 먼저 쓴다.
void ble_peer_select(int slot);
// 이 PC 와의 연결을 끊는다
void ble_peer_disconnect(int slot);

// 블루투스 스피커 시험: 칩의 블루투스 지원 범위를 알리고 10초 동안 주변 기기를 찾아 시리얼로 적는다 ([bt-check])
// target 이 주소(aa:bb:..)면 그 기기에, "all" 이면 연결할 수 있는 기기 모두에 연결해 서비스 목록을 읽는다
void ble_speaker_check(const char *target = nullptr);
