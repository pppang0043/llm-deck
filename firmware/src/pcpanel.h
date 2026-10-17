#pragma once

#include <ArduinoJson.h>
#include <lvgl.h>

// PC 와 함께 쓰는 화면 두 개:
//  - 단축 버튼: 누르면 사용 중인 PC 에서 프로그램을 연다 (엑셀, 파워포인트). PC 도우미가 정해 둔 것만 연다.
//  - PC 상태: 사용 중인 PC 의 CPU, 메모리, 디스크, 네트워크. 화면이 보이는 동안만 PC 가 3초마다 보내 준다.

// 두 화면을 만든다 (ui.cpp 가 부른다). 처음에는 숨겨져 있다.
void pcpanel_build(lv_obj_t *parent);
// 화면 크기가 바뀌면 (돌리기) 다시 배치한다. 상태줄 아래 영역.
void pcpanel_layout(int x, int y, int w, int h);
void pcpanel_show_shortcuts(bool on);
void pcpanel_show_status(bool on);
bool pcpanel_status_visible();

// PC 가 보낸 {"pc": {...}} (link.cpp 가 부른다)
void pcpanel_set_status(JsonObjectConst pc, const char *host);
// loop() 에서 부른다: PC 상태 화면이 보이면 PC 에 계속 보내 달라고 한다
void pcpanel_loop();
