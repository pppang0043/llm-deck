#pragma once

#include <Arduino.h>

// 카메라 (보드 뒷면 24핀 커넥터, OV5640 등).
// 계속 JPEG 로 찍으면서 미리보기는 작은 크기로, 촬영할 때만 큰 크기로 바꾼다.
// 카메라를 껐다 켜지 않으므로 밝기·색 자동 조절이 유지된다.

// 카메라를 켠다. 카메라가 없으면 false.
bool camera_start();
// 카메라를 끈다 (전기와 메모리를 돌려준다).
void camera_stop();
bool camera_running();
// 꽂혀 있는 카메라 이름 (예: "OV5640"). 모르면 "".
const char *camera_model();

// 센서가 보드에 붙은 방향. 미리보기·사진을 바로 세우는 데 쓴다 (기기에 저장).
//   rot: 화면을 돌리지 않았을 때 센서 영상을 시계 방향으로 몇 번(90도) 돌려야 바로 보이는지
//   mirror: 좌우가 뒤집혀 나오면 true
void camera_set_mount(uint8_t rot, bool mirror);
uint8_t camera_mount_rot();
bool camera_mount_mirror();

// 미리보기 한 장을 LVGL 색 형식(RGB565)으로, 화면 방향(screen_rot)에 맞게 돌려 out 에 넣는다.
// fit_w x fit_h 안에 들어가는 크기로 만들고, 실제 크기를 w,h 로 돌려준다.
bool camera_preview(uint16_t *out, int fit_w, int fit_h, uint8_t screen_rot, int *w, int *h);

// 큰 사진(JPEG, 1600x1200)을 찍는다. 화면 방향(screen_rot)에 맞게 바로 보이도록 회전 정보(EXIF)를 넣는다.
// 성공하면 *jpg 에 ps_malloc 한 버퍼 (free 로 해제), *len 에 크기.
bool camera_capture(uint8_t screen_rot, uint8_t **jpg, size_t *len);

// 카메라 없이 저장·전송을 시험하기 위한 무늬 사진(JPEG, 800x600). 버퍼는 free 로 해제.
bool camera_test_pattern(uint8_t **jpg, size_t *len);
