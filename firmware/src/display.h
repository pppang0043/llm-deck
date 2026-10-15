#pragma once

#include <Arduino.h>
#include <SPI.h>

// 화면(ST7789) + 터치(CST816) + LVGL 을 초기화한다.
void display_init();

// loop() 에서 계속 불러줘야 화면이 갱신된다.
void display_loop();

// 화면을 90도 단위로 돌린다 (0~3, 시계 방향). 가로/세로 크기가 바뀐다.
void display_set_rotation(uint8_t rotation);
uint8_t display_rotation();

// 현재 방향에서의 화면 크기
int display_width();
int display_height();

// 마지막으로 화면을 만진 시각 (millis)
uint32_t display_last_touch_ms();
// 만진 것처럼 절전 시간을 처음부터 다시 센다 (버튼·알림이 있을 때)
void display_poke();
// 절전으로 화면이 어둡거나 꺼져 있는 동안 true. 이때의 첫 터치는 깨우기만 하고 버튼을 누르지 않는다.
void display_set_sleeping(bool on);

// 화면 밝기 (10~100%)
void display_set_brightness(uint8_t pct);

// 화면과 SD 카드가 같이 쓰는 SPI
SPIClass &display_spi();
