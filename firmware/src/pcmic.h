#pragma once

#include <ArduinoJson.h>
#include <lvgl.h>

// PC 마이크 표시: 상태줄의 연결 방식 글자 오른쪽에 작은 마이크 모양. PC 도우미가 쉬는 틈에 마이크를 미리 들어 보고 알려 준다
// (companion/assistant.py). 초록: 소리가 오는 마이크가 있다, 노랑: 들어 보는 중, 빨강 ×: 몇 번 들어 봐도 안 돼 연결 불가.
// 누르면 마이크 고르기 창 (자동 + PC 의 마이크들). 고르면 PC 가 그 마이크를 먼저 쓴다.
//   PC -> 기기: {"pc_mic": {"state": "ok" | "checking" | "none", "name": 쓰는 마이크, "why": 연결 불가 까닭,
//                           "list": [고를 수 있는 마이크...], "pick": 고른 마이크 ("" 이면 자동),
//                           "limited": [고르면 30분만 쓰는 마이크 (Mac 의 아이폰)], "left": 고른 그 마이크의 남은 분}}
//   기기 -> PC: {"mic_pick": 이름} ("" 이면 자동)
// PC 가 연결되어 있지 않거나 아직 알려 주지 않았으면 (옛 도우미) 숨긴다.

void pcmic_build(lv_obj_t *scr);
// 상태줄에서 left 오른쪽에 둔다. 차지한 오른쪽 끝 x (숨었으면 left 의 오른쪽 끝)
int pcmic_place(lv_obj_t *left);
void pcmic_set(JsonObjectConst mic);
// 연결이 바뀌었을 때 (끊기면 숨기고 PC 가 다시 알려 줄 때까지 모른다)
void pcmic_refresh();
// 연결 불가 까닭 (모르면 빈 글자)
const char *pcmic_why();
