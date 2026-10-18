#pragma once

#include <ArduinoJson.h>
#include <lvgl.h>

// ORCA 화면: PC 의 ORCA 에 열린 세션들과 작업 상태 (PC 도우미가 10초마다 읽어 바뀌면 보낸다, companion/orca.py).
// 세션을 누르면 PC 가 그 세션을 앞으로 띄우고 받아쓰기를 시작한다. 다시 누르면 받아 적은 글을 그 세션에 넣는다 (Enter 없이).
//   PC -> 기기: {"orca": {"ok": bool, "list": [{"id", "t": 이름, "p": 프로젝트, "s": 상태}]}}
//   기기 -> PC: {"ask": "start", "mode": "dictate", "orca": id} ... {"ask": "stop"} (askpanel.cpp)

void orcapanel_build(lv_obj_t *parent);
// 상태줄 아래 영역. 화면을 돌리면 다시 부른다.
void orcapanel_layout(int x, int y, int w, int h);
void orcapanel_show(bool on);
// PC 가 보낸 {"orca": {...}} (link.cpp 가 부른다)
void orcapanel_set(JsonObjectConst orca);
// 받아쓰기 진행이나 연결이 바뀌었을 때 다시 그린다 (ui_refresh_talk 가 부른다)
void orcapanel_refresh();
