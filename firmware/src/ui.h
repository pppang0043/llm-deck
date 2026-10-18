#pragma once

#include <ArduinoJson.h>

// 화면 구성: 위쪽 상태줄(오른쪽 끝에 화면 돌리기 버튼) / 가운데 서비스별 페이지(위아래로 넘김, 맨 끝은 PC 상태) / 질문 버튼
void ui_init(void (*on_mic_toggle)(), void (*on_rotate)());

// 상태줄 왼쪽 글자 (현재 연결 방식)
void ui_set_status(const char *text);

// 상태줄 오른쪽(배터리 옆)의 날짜와 시각. 자리가 모자라면 날짜는 빼고 시각만 보인다.
void ui_set_clock(const char *date, const char *time);

// 상태줄 오른쪽(화면 돌리기 버튼 옆)의 배터리 잔량. -1 이면 숨긴다. 충전 중이면 건전지 안에 번개를 그린다.
void ui_set_battery(int pct, bool charging);

// 대기 화면에 보여줄 Wi-Fi 안내 (IP 주소 등)
void ui_set_wifi_info(const char *text);

// PC 도우미가 보낸 사용량 데이터로 페이지를 다시 그린다.
// 서비스 하나: {"name", "note", "headline", "sub", "bars": [{"label", "note", "text", "pct"}], "rows": [[이름, 값]]}
void ui_set_usage(JsonArrayConst services);

// 화면 방향이 바뀐 뒤(display_set_rotation) 부르면 가로/세로에 맞게 다시 배치한다.
void ui_layout();

void ui_set_mic(bool on);
// 사용량 화면 아래 받아쓰기 버튼의 모양을 다시 그린다 (askpanel.cpp 가 부른다)
void ui_refresh_talk();

// 홈 화면 (아이콘 5개: 사용량, 카메라, 질문, 단축키, 설정). 어디서든 오른쪽으로 밀면 홈으로 온다.
// 밝기와 정보는 설정 안(디스플레이, 정보)에 있다.
void ui_show_home();
// 홈의 카메라 아이콘을 눌렀을 때 할 일
void ui_set_home_actions(void (*open_camera)());
// 설정 > 정보 화면이 보이는 중인지, 그 내용
bool ui_info_visible();
bool ui_settings_visible();
// 모든 화면 위에 잠깐 뜨는 알림 (ms 뒤에 사라진다)
void ui_toast(const char *text, uint32_t ms = 4000);

// 화면 바꾸기: "home" / "usage" / "info"(설정 > 정보) / "settings" / "shortcuts"(= "programs") / "keywords" / "pc" / "ask" (PC 에서 시험할 때)
void ui_show(const char *name);
// 지금 화면을 RGB565 로 찍는다 (진단용). *pixels 는 free 로 해제.
bool ui_screenshot(uint16_t **pixels, int *w, int *h);
void ui_set_info(const char *text);
// 화면 보호기에 띄울 짧은 사용률 요약 (서비스마다 한 줄). 받은 사용량이 없으면 빈 글자.
void ui_usage_summary(char *out, size_t size);
void ui_set_level(uint8_t pct);

// 사용량 구간을 눌러 연 기록 그래프 (화면 전체). 다른 화면으로 갈 때 닫는다.
bool ui_history_visible();
void ui_history_close();
