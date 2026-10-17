#pragma once

#include <Arduino.h>
#include <lvgl.h>

// 기기 설정. 바꾸면 바로 적용하고 기기에 저장한다 (껐다 켜도 유지).
void settings_init();

uint8_t settings_brightness();
void settings_set_brightness(uint8_t pct);
// 홈의 밝기 아이콘: 100 -> 60 -> 30 -> 100%
void settings_cycle_brightness();

// 기울이면 화면이 따라 도는지
bool settings_auto_rotate();
// 마이크를 켜고 끌 때 PC 스피커로 알림음을 낼지
bool settings_mic_sound();
// 배터리로만 쓸 때 만지지 않으면 화면을 어둡게(1분) 하고 끄는지(3분)
bool settings_auto_dim();
// 만지지 않은 지 이만큼 지나면 화면 보호기를 띄운다 (0 이면 끔)
uint32_t settings_saver_ms();
// 사용률이 80%, 90% 를 넘거나 초기화되면 알릴지
bool settings_limit_alerts();

// 연결 방식을 쓸지 (link.h 의 Link 값: USB / 블루투스 / Wi-Fi / 클라우드). 끈 방식으로 오는 데이터는 받지 않는다.
bool settings_link_allowed(int link);
void settings_set_link_allowed(int link, bool allowed);

// 찍은 사진을 SD 카드에도 저장할지
bool settings_sd_photos();

// 설정 화면이 보이는 동안 loop() 에서 부른다 (연결 상태 등을 1초마다 고친다)
void settings_loop();
// 지금 바로 상태 글자를 고친다 (화면 찍기 직전)
void settings_redraw();
// 분류 화면에 들어가 있으면 목록으로 돌아가고 true (오른쪽으로 밀기)
bool settings_back();
// 분류 화면 바로 열기: "wifi" / "bluetooth" / "notify" / "display" / "battery" / "camera" / "sd" / "general"
// "end" 는 지금 화면의 맨 아래로 (진단용)
void settings_open(const char *name);

// 설정 화면을 parent 안에 만든다 (ui.cpp 가 부른다)
void settings_build(lv_obj_t *parent);
// 설정 화면이 보일 때마다 불러 값을 다시 맞춘다
void settings_refresh();
