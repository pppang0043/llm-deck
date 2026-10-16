#pragma once

#include <Arduino.h>

// 화면 보호기: 오래 만지지 않으면 검은 바탕에 시계·사용률만 어둡게 띄우고, 30초마다 자리를 옮긴다.
// LCD 는 같은 그림을 오래 띄워 두면 자국(잔상)이 남을 수 있고, 백라이트도 밝을수록 빨리 닳는다.
void saver_init();

// main 이 켜고 끈다 (절전 상태와 함께 판단)
void saver_set_visible(bool on);
bool saver_visible();
// 화면 방향이 바뀐 뒤 부른다
void saver_layout();

// loop() 에서 부른다. 보이는 동안 시계를 고치고 자리를 옮긴다.
void saver_loop();

// 진단용: 지금 바로 보호기를 띄운다. 화면을 만지면 사라진다.
void saver_demo();
bool saver_demo_active();
