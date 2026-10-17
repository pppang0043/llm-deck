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
