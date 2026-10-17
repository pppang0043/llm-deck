#pragma once

// LLM Deck 보드 <-> 스피커 모듈(ATOM Lite) I2C 약속. 두 펌웨어가 같이 쓴다.
// 보드가 마스터, 모듈이 슬레이브. 터치 센서(0x15), 기울기 센서(0x6B)와 같은 선을 나눠 쓴다.
//
// 보드가 쓰는 것: [명령 1바이트][값...]
// 보드가 읽는 것: SPK_CMD_SELECT 로 고른 SPK_REPLY_LEN 바이트 (상태 또는 찾은 스피커 하나)

#define SPK_ADDR 0x2A
#define SPK_PROTO 1
#define SPK_REPLY_LEN 36
#define SPK_MAX_FOUND 8
#define SPK_NAME_LEN 30

// 명령
#define SPK_CMD_PLAY 'P'    // [소리 번호] 재생. 쉬고 있으면 깨어나 연결한 뒤 낸다.
#define SPK_CMD_FIND 'F'    // 주변 스피커 찾기 (약 1분)
#define SPK_CMD_CONNECT 'C' // [찾은 목록 번호] 그 스피커를 기억하고 연결
#define SPK_CMD_FORGET 'X'  // 기억한 스피커 지우고 쉬기
#define SPK_CMD_CONFIG 'K'  // [항상 연결 0/1][소리 크기 0~100]
#define SPK_CMD_SELECT 'S'  // [0xFF=상태, 0~7=찾은 스피커] 다음에 읽을 내용 고르기

// 소리 번호
#define SPK_SOUND_MIC_ON 1
#define SPK_SOUND_MIC_OFF 2
#define SPK_SOUND_LIMIT_WARN 3
#define SPK_SOUND_LIMIT_RESET 4
#define SPK_SOUND_TEST 5

// 상태
#define SPK_STATE_OFF 0         // 블루투스 꺼짐 (쉬는 중)
#define SPK_STATE_CONNECTING 1  // 기억한 스피커에 연결하는 중
#define SPK_STATE_FINDING 2     // 주변 스피커 찾는 중
#define SPK_STATE_CONNECTED 3   // 연결됨
#define SPK_STATE_PLAYING 4     // 소리 내는 중

// 상태 응답: [0]'A' [1]SPK_PROTO [2]상태 [3]찾은 수 [4]항상 연결 [5]소리 크기
//            [6]기억한 스피커 있음 [7]예비 [8..]기억한 스피커 이름 (SPK_NAME_LEN, 끝 0)
// 찾은 스피커 응답: [0]'E' [1]있음 [2]신호(int8 dBm) [3]예비 [4..]이름 (SPK_NAME_LEN, 끝 0)
