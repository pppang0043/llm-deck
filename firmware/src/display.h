#pragma once

// 화면(ST7789) + 터치(CST816) + LVGL 을 초기화한다.
void display_init();

// loop() 에서 계속 불러줘야 화면이 갱신된다.
void display_loop();
