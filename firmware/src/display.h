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

// 화면과 SD 카드가 같이 쓰는 SPI
SPIClass &display_spi();
