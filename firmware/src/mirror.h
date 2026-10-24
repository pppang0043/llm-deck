#pragma once

#include <Arduino.h>

// 미러링: PC 의 미러링 창(companion/mirror.py)에 기기 화면을 보내고, 창에서 누른 것을 기기 터치로 받는다.
// USB 또는 Wi-Fi (Wi-Fi 는 동영상처럼 기기가 PC 의 TCP 4011 번에 접속해 같은 묶음 형식으로 보낸다).
//   PC -> 기기: {"mirror": true} 창이 열려 있는 동안 2초마다 (6초 동안 안 오면 끈다)
//              {"touch": {"x", "y", "down"}} 창에서 누르기·끌기·떼기 (좌표는 지금 화면 방향 기준)
//              {"touch": [{...}, ...]} 휠·키보드로 밀기: 누르기부터 떼기까지 한 번에 (기기가 10ms 마다 하나씩 쓴다)
//   기기 -> PC: 화면이 바뀌면 한 장을 USB_FRAME_MIRROR 조각으로 [장 번호 4][위치 4][장 크기 4][폭 2][높이 2][데이터]
//              데이터는 같은 색이 이어지는 것만 줄인 RGB565 (mirror.cpp 의 pack, 브라우저가 푼다)
// 평소에는 기기 LCD 와 PC 창이 같은 화면이다. 구독제 화면의 작은 "분리" 버튼으로 나눠 쓰기를 켜면
// 기기 LCD 에는 사용량(구독 한도 표)을 따로 보여 주고, PC 창에서 화면을 옮겨도 기기는 그대로 있다.
#define USB_FRAME_MIRROR 5

void mirror_init();
// PC 가 보낸 {"mirror": on}
void mirror_keep(bool on);
// loop() 에서 부른다: 켜져 있으면 바뀐 화면을 보낸다 (초당 최대 약 25장)
void mirror_loop();
// 미러링 창이 열려 있는지
bool mirror_active();
// 나눠 쓰기 (켜 둔 것은 미러링이 다시 시작돼도 그대로)
bool mirror_split();
void mirror_set_split(bool on);
