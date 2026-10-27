#pragma once
#include <lvgl.h>

// 상단에서 아래로 쓸면 빠른 설정, 위로 쓸거나 바깥/닫기를 누르면 닫는다.
void quickpanel_init(void (*sleep_handler)());
void quickpanel_layout();
// LVGL 에 입력을 넘기기 전에 부른다. true 이면 이번 쓸기는 버튼 입력으로 넘기지 않는다.
bool quickpanel_touch(lv_disp_t *display, int x, int y, bool down);
