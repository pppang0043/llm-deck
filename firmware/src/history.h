#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// 사용률 기록: 서비스·구간(예: "Claude Code" 의 "5시간")마다 최근 24시간을 10분 칸으로 모은다.
// 30분마다 기기에 저장해 다시 켜도 남는다. 시계가 맞춰지기 전에는 기록하지 않는다.

#define HISTORY_SLOTS 144     // 24시간 / 10분
#define HISTORY_SLOT_S 600    // 한 칸 = 10분
#define HISTORY_NONE 255      // 그 칸에 값이 없음

void history_init();
// 화면에 보여 줄 사용량을 받을 때마다 부른다 (ui_set_usage)
void history_record(JsonArrayConst services);
// loop() 에서 부른다 (바뀐 기록을 30분마다 저장)
void history_loop();

// service 의 label 구간 기록을 오래된 것부터 out 에 HISTORY_SLOTS 개 채운다 (값이 없으면 HISTORY_NONE).
// 맨 끝 칸이 지금. 기록이 없으면 false.
bool history_get(const char *service, const char *label, uint8_t *out);
