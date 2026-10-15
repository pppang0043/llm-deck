#pragma once

#include <ArduinoJson.h>

// 화면 구성: 위쪽 상태줄(오른쪽 끝에 화면 돌리기 버튼) / 가운데 서비스별 페이지(좌우로 넘김) / 마이크 버튼
void ui_init(void (*on_mic_toggle)(), void (*on_rotate)());

// 상태줄 왼쪽 글자 (현재 연결 방식)
void ui_set_status(const char *text);

// 상태줄 오른쪽(배터리 옆)의 날짜와 시각. 자리가 모자라면 날짜는 빼고 시각만 보인다.
void ui_set_clock(const char *date, const char *time);

// 상태줄 오른쪽(화면 돌리기 버튼 옆)의 배터리 잔량. -1 이면 숨긴다.
void ui_set_battery(int pct);

// 대기 화면에 보여줄 Wi-Fi 안내 (IP 주소 등)
void ui_set_wifi_info(const char *text);

// PC 도우미가 보낸 사용량 데이터로 페이지를 다시 그린다.
// 서비스 하나: {"name", "note", "headline", "sub", "bars": [{"label", "note", "text", "pct"}], "rows": [[이름, 값]]}
void ui_set_usage(JsonArrayConst services);

// 화면 방향이 바뀐 뒤(display_set_rotation) 부르면 가로/세로에 맞게 다시 배치한다.
void ui_layout();

void ui_set_mic(bool on);
void ui_set_level(uint8_t pct);
