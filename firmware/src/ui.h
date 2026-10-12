#pragma once

#include <ArduinoJson.h>

// 화면 구성: 위쪽 상태줄 / 가운데 서비스별 페이지(좌우로 넘김) / 아래쪽 마이크 버튼
void ui_init(void (*on_mic_toggle)());

// 상태줄 왼쪽 글자 (현재 연결 방식)
void ui_set_status(const char *text);

// 대기 화면에 보여줄 Wi-Fi 안내 (IP 주소 등)
void ui_set_wifi_info(const char *text);

// PC 도우미가 보낸 사용량 데이터로 페이지를 다시 그린다.
void ui_set_usage(JsonArrayConst services, const char *updated);

void ui_set_mic(bool on);
void ui_set_level(uint8_t pct);
