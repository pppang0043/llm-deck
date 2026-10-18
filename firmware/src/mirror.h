#pragma once

#include <Arduino.h>

// 미러링: PC 의 미러링 창(companion/mirror.py)에 기기 화면을 보내고, 창에서 누른 것을 기기 터치로 받는다. USB 로만.
//   PC -> 기기: {"mirror": true} 창이 열려 있는 동안 2초마다 (6초 동안 안 오면 끈다)
//              {"touch": {"x", "y", "down"}} 창에서 누르기·끌기·떼기 (좌표는 지금 화면 방향 기준)
//   기기 -> PC: 화면이 바뀌면 JPEG 한 장을 USB_FRAME_MIRROR 조각으로 [장 번호 4][위치 4][장 크기 4][폭 2][높이 2][데이터]
#define USB_FRAME_MIRROR 5

void mirror_init();
// PC 가 보낸 {"mirror": on}
void mirror_keep(bool on);
// loop() 에서 부른다: 켜져 있으면 바뀐 화면을 보낸다 (초당 최대 약 7장)
void mirror_loop();
