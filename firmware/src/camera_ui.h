#pragma once

// 카메라 화면. 사용량 화면에서 위로 밀면 열리고, 아래로 밀면 닫힌다.
// 미리보기를 누르면 사진을 찍어 SD 카드와 PC 에 저장한다 (photo.h).
void camera_ui_init();
void camera_ui_loop();
// 화면 방향이 바뀌었을 때
void camera_ui_layout();
bool camera_ui_open();
