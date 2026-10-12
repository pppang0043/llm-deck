// Waveshare ESP32-S3-Touch-LCD-2 핀 정의.
// 출처: Waveshare 공식 데모(ESP32-S3-Touch-LCD-2-Demo.zip)와 위키 핀아웃 그림.
#pragma once

// LCD (ST7789T3, SPI)
#define LCD_SCLK 39
#define LCD_MOSI 38
#define LCD_MISO 40
#define LCD_DC 42
#define LCD_CS 45
#define LCD_RST -1
#define LCD_BL 1
#define LCD_W 240
#define LCD_H 320

// 터치 (CST816D, I2C)
#define TP_SDA 48
#define TP_SCL 47

// BOOT 버튼: 누르면 LOW
#define BTN_BOOT 0

// 외장 I2S 마이크 (INMP441). 카메라가 쓰지 않는 헤더 핀만 사용한다.
// GPIO43/44 는 헤더에 TXD/RXD 로 표기된 핀 (USB 로 로그를 보내므로 비어 있음).
#define MIC_SCK 18
#define MIC_WS 43
#define MIC_SD 44
