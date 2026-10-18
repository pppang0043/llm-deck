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

// 미러링 (PC 의 미러링 창이 열려 있는 동안 켠다, mirror.h). 켜면 화면에 그리는 그림을 PSRAM 에 모아 둔다.
void display_mirror(bool on);
// 지난번 뒤로 화면이 바뀌었으면 지금 화면을 out (320x240x2 바이트 이상) 에 복사한다. 픽셀은 LVGL 색 형식(RGB565).
bool display_mirror_take(uint16_t *out, int *w, int *h);
// PC 에서 누른 것. 좌표는 지금 화면 방향 기준. 누르기, 끌기, 떼기를 차례로 넣는다.
void display_remote_touch(int x, int y, bool down);

// 화면 밝기 (10~100%)
void display_set_brightness(uint8_t pct);

// 화면과 SD 카드가 같이 쓰는 SPI
SPIClass &display_spi();

// 진단용: I2C 데이터 선을 낮게 잡아 막힌 것처럼 만든다. 터치가 1초쯤 안 읽히다가 스스로 풀려야 한다 ([i2c] 로그).
void display_i2c_test();
