#pragma once

#include <Arduino.h>

// 재시작 원인 기록. 프로그램이 멈추거나(패닉, 감시 장치) 전압이 떨어져 다시 켜지면
// 그 이유와 그 전에 얼마나 켜져 있었는지, 내부 RAM 이 가장 적었을 때를 기기에 저장해 둔다.

// setup() 맨 앞에서 부른다 (지난번 기록을 읽고 이번 원인을 적는다)
void health_init();
// loop() 에서 부른다 (켜진 시간과 내부 RAM 최저치를 재시작해도 남는 곳에 적어 둔다)
void health_loop();

// 이번에 켜진 이유: "전원 켬" / "재시작" / "프로그램 멈춤" 등
const char *health_boot_reason();
// 마지막 오류 재시작: "10/18 01:20 프로그램 멈춤 (3시간 12분 켜진 뒤)". 오류가 없었으면 false.
bool health_last_error(char *out, size_t size);
// 지금까지 오류로 다시 켜진 횟수
uint32_t health_error_count();
// 오류 기록을 지운다 (시험 뒤)
void health_clear();

// 감시 장치: loop() 가 한계시간(20초) 넘게 돌아오지 않으면(어딘가에서 멈춤) 기기를 다시 켠다.
// 다시 켜진 이유는 "응답 없음 (감시 장치)" 로 남고, 사용량·연결은 저장해 둔 값으로 곧 돌아온다.
// setup() 끝에서 켠다 (부팅 중의 긴 준비는 재지 않는다)
void health_watch_start();
// loop() 마다, 그리고 오래 걸리는 일(파일 여러 개 지우기, 무선 업데이트) 중간중간 부른다
void health_watch_feed();
// 한 번에 끝나는 긴 일(SD 카드 포맷) 동안만 잠깐 재지 않는다
void health_watch_pause(bool paused);
