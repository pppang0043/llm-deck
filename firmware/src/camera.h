#pragma once

#include <Arduino.h>

// 카메라 (보드 뒷면 24핀 커넥터). 미리보기는 RGB565 작은 크기로, 촬영은 큰 JPEG 로 잠깐 바꿔서 찍는다.

// 미리보기를 시작한다. landscape 면 320x240, 아니면 240x176. 카메라가 없으면 false.
bool camera_start(bool landscape);
// 화면 방향이 바뀌었을 때 미리보기 크기를 바꾼다.
void camera_set_landscape(bool landscape);
// 카메라를 끈다 (전기와 메모리를 돌려준다).
void camera_stop();
bool camera_running();
// 꽂혀 있는 카메라 이름 (예: "OV2640"). 모르면 "".
const char *camera_model();

// 미리보기 한 장을 LVGL 색 형식(RGB565, 이 보드 바이트 순서)으로 out 에 복사한다. 크기를 w,h 로 돌려준다.
bool camera_preview(uint16_t *out, size_t max_pixels, int *w, int *h);

// 큰 사진(JPEG)을 찍는다. 성공하면 *jpg 에 ps_malloc 한 버퍼 (free 로 해제), *len 에 크기.
bool camera_capture(uint8_t **jpg, size_t *len);

// 카메라 없이 저장·전송을 시험하기 위한 무늬 사진(JPEG, 800x600). 버퍼는 free 로 해제.
bool camera_test_pattern(uint8_t **jpg, size_t *len);
