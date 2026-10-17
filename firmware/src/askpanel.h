#pragma once

#include <ArduinoJson.h>
#include <lvgl.h>

// 질문 화면: 아래 버튼을 누르고 있는 동안 PC 마이크로 말하고, 떼면 PC 도우미가 받아 적어 Claude 에 묻는다.
// 답은 글자로 나오는 대로 이 화면에 뜬다 (companion/assistant.py).
//   기기 -> PC: {"ask": "start"} 누름 / "stop" 뗌 / "cancel" 버림 / "new" 새 대화
//              음소거를 풀었으면 {"ask": "stop", "voice": true} (답을 PC 스피커로 읽어 줌), 다시 음소거하면 {"ask": "mute"}
//   PC -> 기기: {"ask": {"state": "listening" | "thinking" | "done" | "error" | "idle", "q": 질문, "a": 답, "done": bool}}

void askpanel_build(lv_obj_t *parent);
// 상태줄 아래 영역. 화면을 돌리면 다시 부른다.
void askpanel_layout(int x, int y, int w, int h);
void askpanel_show(bool on);
// PC 가 보낸 {"ask": {...}} (link.cpp 가 부른다)
void askpanel_set(JsonObjectConst ask);

// 말하기 버튼은 두 곳에 있다: 이 화면 아래, 사용량 화면 아래(ui.cpp). 상태는 여기서 하나로 관리한다.
enum { ASK_READY, ASK_TALKING, ASK_NO_PC, ASK_NO_MIC };
int askpanel_talk_state();
// 누름: 녹음을 시작하면 true (PC 가 없거나 마이크가 없으면 알림만 띄우고 false). 뗌: 질문을 보냈으면 true.
bool askpanel_press();
bool askpanel_release();
// PC 가 알려 준 마이크 사용 가능 여부 {"ask_mic": bool}
void askpanel_set_mic(bool available);
// 연결이 바뀌었을 때 등 버튼 모양을 다시 그린다
void askpanel_refresh();
// loop() 에서 부른다: PC 가 "듣는 중/답 받는 중" 뒤로 오래 말이 없으면 그 표시를 지운다
void askpanel_loop();
