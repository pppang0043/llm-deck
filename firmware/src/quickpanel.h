#pragma once
#include <lvgl.h>

// 상단에서 아래로 쓸면 빠른 설정, 위로 쓸거나 바깥/닫기를 누르면 닫는다.
// sleep_handler: 절전 모드 버튼, drive_handler: USB 드라이브 버튼
void quickpanel_init(void (*sleep_handler)(), void (*drive_handler)());
void quickpanel_layout();
// ORCA 완료 알림: 양쪽 화면 가운데 팝업. 팝업 노출은 읽음이 아니며 상단 목록을 열면 읽음 처리.
void quickpanel_notify(const char *text);
unsigned quickpanel_unread_count();
// 상태줄의 오른쪽 요소 앞에 알림 아이콘/개수를 놓고 그 요소를 돌려준다 (미확인 0개면 원래 요소).
lv_obj_t *quickpanel_status_before(lv_obj_t *right);
// LVGL 에 입력을 넘기기 전에 부른다. true 이면 이번 쓸기는 버튼 입력으로 넘기지 않는다.
bool quickpanel_touch(lv_disp_t *display, int x, int y, bool down);
