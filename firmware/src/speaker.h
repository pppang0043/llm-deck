#pragma once

#include <Arduino.h>

// 블루투스 스피커 모듈 (M5Stack ATOM Lite, 폴더 speaker/). 터치 센서와 같은 I2C 선에 붙는다.
// 이 보드(ESP32-S3)는 블루투스 스피커를 직접 못 쓰므로 모듈에 "이 소리 내 줘" 만 보내고,
// 소리는 모듈이 만들어 스피커로 보낸다. 모듈이 없으면 모든 함수가 아무 일도 하지 않는다.

void speaker_init();
// loop() 에서 부른다. fast 면 상태를 자주 읽는다 (설정의 스피커 화면이 보일 때).
void speaker_loop(bool fast);

// 모듈이 선에 붙어 있는지
bool speaker_present();
// speaker_proto.h 의 SPK_STATE_*. 모듈이 없으면 -1.
int speaker_state();
// 기억한 스피커가 있으면 이름을 넣고 true
bool speaker_target(char *name, size_t size);

// 알림음 이름("mic_on" / "mic_off" / "limit_warn" / "limit_reset" / "test")을 스피커로 낸다.
// 설정에서 스피커를 켰고 모듈과 기억한 스피커가 있으면 true (그러면 PC 로는 보내지 않는다).
bool speaker_play(const char *sound);

// 주변 스피커 찾기 (약 1분). 찾은 것은 speaker_found 로 읽는다.
void speaker_find();
int speaker_found_count();
// i 번째 찾은 스피커. 없으면 false.
bool speaker_found(int i, char *name, size_t size, int *rssi);
// i 번째 찾은 스피커를 기억하고 연결한다 (연결되면 시험 소리가 난다)
void speaker_connect(int i);
// 기억한 스피커를 지운다
void speaker_forget();
