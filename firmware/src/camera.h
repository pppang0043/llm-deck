#pragma once

#include <Arduino.h>

// 카메라 (보드 뒷면 24핀 커넥터, OV5640 등).
// 계속 JPEG 로 찍으면서 미리보기는 작은 크기로, 촬영할 때만 큰 크기로 바꾼다.
// 카메라를 껐다 켜지 않으므로 밝기·색 자동 조절이 유지된다.

// 부팅 때 한 번: 카메라가 켜질 때 쓸 내부 RAM 자리를 미리 잡아 둔다.
void camera_init();
// 카메라를 쓰지 않는 동안 잡아 둔 자리를 잠깐 내준다 (HTTPS 연결처럼 내부 RAM 이 많이 필요한 일 앞뒤로).
// lend=true 로 내주고, 끝나면 lend=false 로 다시 잡는다.
void camera_lend_reserve(bool lend);

// 카메라를 켠다. 카메라가 없으면 false.
bool camera_start();
// 카메라를 끈다 (전기와 메모리를 돌려준다).
void camera_stop();
bool camera_running();
// 꽂혀 있는 카메라 이름 (예: "OV5640"). 모르면 "".
const char *camera_model();
// 마지막으로 켜지 못한 이유가 "카메라 없음" 인지 (아니면 메모리 부족 등)
bool camera_missing();

// 센서가 보드에 붙은 방향. 미리보기·사진을 바로 세우는 데 쓴다 (기기에 저장).
//   rot: 화면을 돌리지 않았을 때 센서 영상을 시계 방향으로 몇 번(90도) 돌려야 바로 보이는지
//   mirror: 좌우가 뒤집혀 나오면 true
void camera_set_mount(uint8_t rot, bool mirror);
uint8_t camera_mount_rot();
bool camera_mount_mirror();

// 화질 조절 (카메라 드라이버의 조절 기능 전부, 방향·좌우 반전은 위의 mount 로). 바꾼 값은 기기에 저장해
// 카메라를 켤 때마다 다시 넣는다. 값은 카메라가 켜져 있을 때만 읽을 수 있다.
int camera_ctl_count();
const char *camera_ctl_name(int i);
bool camera_ctl_value(int i, int *v);
// dir 만큼(+1 / -1) 한 칸 바꾼다. 드라이버가 받아들이지 않으면 false
bool camera_ctl_step(int i, int dir);
// 값을 화면에 보일 글로 ("자동", "-1", "켬" 처럼)
void camera_ctl_text(int i, int v, char *out, size_t size);
// 이름(CTLS 의 key)으로 값을 정한다 (PC 에서). 꺼져 있으면 저장만 하고 켤 때 넣는다. "reset" 이면 모두 기본값
bool camera_ctl_set(const char *key, int v);
// 저장한 값을 모두 지우고 드라이버 기본값으로 (켜져 있으면 카메라를 다시 켠다)
void camera_ctl_reset();

// 미리보기 한 장을 LVGL 색 형식(RGB565)으로, 화면 방향(screen_rot)에 맞게 돌려 out 에 넣는다.
// fit_w x fit_h 안에 들어가는 크기로 만들고, 실제 크기를 w,h 로 돌려준다.
// 녹화 중에는 녹화한 가장 최근 장을 줄여서 보여준다.
bool camera_preview(uint16_t *out, int fit_w, int fit_h, uint8_t screen_rot, int *w, int *h);

// 녹화 모드: 따로 도는 태스크가 640x480 JPEG 를 최대한 빨리 찍어 frame_cb 로 넘긴다 (그 태스크에서 불린다).
void camera_set_video(bool on, void (*frame_cb)(const uint8_t *jpg, size_t len));

// 화면 방향(screen_rot)에서 센서 영상을 시계 방향으로 몇 번(90도) 돌려야 바로 보이는지
uint8_t camera_turns(uint8_t screen_rot);

// 큰 사진(JPEG, 2560x1920)을 찍는다. 화면 방향(screen_rot)에 맞게 바로 보이도록 회전 정보(EXIF)를 넣는다.
// 성공하면 *jpg 에 ps_malloc 한 버퍼 (free 로 해제), *len 에 크기.
// focus: 찍기 직전에 한 번 초점을 맞춘다 (1.2초). 연속 스캔은 첫 장만 맞추고 나머지는 연속 초점에 맡긴다
bool camera_capture(uint8_t screen_rot, uint8_t **jpg, size_t *len, bool focus = true);
// QR·바코드 스캔 (2단계):
//   1. camera_scan_locate: 화각 전체를 1024x768 로 한 장 (PC 가 바코드 자리를 찾는다)
//   2. camera_scan_zoom: 그 자리(1단계 장 기준 0~1000)를 센서 원본 화소로 잘라 읽는 창으로 바꾸고,
//      렌즈를 직접 옮겨 그 영역이 가장 선명한 곳에 둔다. 이후 camera_scan_grab 으로 장을 바로 꺼낸다 (10ms 안팎)
//   camera_scan_end: 미리보기 크기로 돌아간다
bool camera_scan_locate(uint8_t screen_rot, uint8_t **jpg, size_t *len);
bool camera_scan_zoom(int x0, int y0, int x1, int y1);
bool camera_scan_grab(uint8_t screen_rot, uint8_t **jpg, size_t *len);
void camera_scan_end();

// 카메라 없이 저장·전송을 시험하기 위한 무늬 사진(JPEG, 800x600). 버퍼는 free 로 해제.
bool camera_test_pattern(uint8_t **jpg, size_t *len);
