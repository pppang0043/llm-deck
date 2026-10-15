#pragma once

// 카메라 화면. 홈의 카메라 아이콘으로 열고, 오른쪽으로 밀면 홈으로(아래로 밀면 닫기).
// 미리보기를 누르면 사진을 찍어 SD 카드와 PC 에 저장한다 (photo.h).
void camera_ui_init();
void camera_ui_show();
void camera_ui_loop();
// 화면 방향이 바뀌었을 때
void camera_ui_layout();
bool camera_ui_open();
// PC 에서 카메라 화면을 다룬다: "open" / "shoot" / "close"
void camera_ui_command(const char *cmd);
