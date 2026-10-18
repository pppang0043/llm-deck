#pragma once

#include <ArduinoJson.h>
#include <lvgl.h>

// PC 와 함께 쓰는 화면 두 개. 단축키는 PC 도우미가 정해 둔 것만 연다·입력한다
// (기기가 아무 명령·글이나 보내지 못하게).
//  - 단축키: 왼쪽 끝 칸에서 "프로그램" / "키워드" 를 고른다.
//      프로그램: 누르면 사용 중인 PC 에서 연다 (엑셀, 파워포인트, ORCA, 파워쉘, 관리자 파워쉘).
//      키워드: 누르면 PC 에서 지금 커서가 있는 곳에 정해 둔 글을 입력하고 Enter 를 누른다 (커밋해줘 등).
//  - PC 상태: 사용 중인 PC 의 CPU, 메모리, 디스크, 네트워크. 사용량 화면의 맨 마지막 페이지이고,
//    그 페이지가 보이는 동안만 PC 가 3초마다 보내 준다.

// 두 화면을 만든다 (ui.cpp 가 부른다). 처음에는 숨겨져 있다.
void pcpanel_build(lv_obj_t *parent);
// 화면 크기가 바뀌면 (돌리기) 다시 배치한다. 상태줄 아래 영역.
void pcpanel_layout(int x, int y, int w, int h);
void pcpanel_show_shortcuts(bool on);
// 단축키 화면에서 고를 묶음: 0 = 프로그램, 1 = 키워드
void pcpanel_select_shortcuts(int page);
// PC 상태는 사용량 페이지들의 맨 마지막 페이지다 (ui.cpp). 페이지를 다시 만들기 전에 떼어 두고, 만든 뒤 붙인다.
void pcpanel_status_attach(lv_obj_t *tile, int w, int h);
void pcpanel_status_detach();
// PC 상태 페이지가 지금 보이는지 (보이는 동안만 PC 에 계속 보내 달라고 한다)
void pcpanel_show_status(bool on);
bool pcpanel_status_visible();

// 사용 중인 PC 의 종류 ("mac", "win", "linux"). Mac 이면 파워쉘 버튼을 "터미널" 로 보이고 관리자 버튼은 뺀다
// (Mac 도우미는 둘 다 터미널을 연다)
void pcpanel_set_os(const char *os);
// PC 가 보낸 {"pc": {...}} (link.cpp 가 부른다)
void pcpanel_set_status(JsonObjectConst pc, const char *host);
// loop() 에서 부른다: PC 상태 화면이 보이면 PC 에 계속 보내 달라고 한다
void pcpanel_loop();
