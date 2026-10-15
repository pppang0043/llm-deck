// LVGL 8.4 설정. 여기 없는 항목은 LVGL 기본값을 쓴다.
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

// 화면 요소는 PSRAM 에 둔다. 내부 RAM 은 카메라·Wi-Fi·블루투스가 쓰는 연속된 덩어리(DMA)를 위해 남겨 둔다.
// (작은 할당도 내부 RAM 에 잡으면 오래 켜 둔 뒤 잘게 쪼개져 카메라가 켜지지 않았다)
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <esp_heap_caps.h>
#define LV_MEM_CUSTOM_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
#define LV_MEM_CUSTOM_FREE heap_caps_free
#define LV_MEM_CUSTOM_REALLOC(p, size) heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM)

#define LV_TICK_CUSTOM 1

// 화면 캡처 (진단용, PC: --screenshot)
#define LV_USE_SNAPSHOT 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

// 10: 건전지 모양 안의 작은 번개 표시
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#endif
