#pragma once

#include <Arduino.h>

// PC 와 연결하는 세 가지 방법. 셋 다 항상 켜져 있고,
// 가장 최근에 데이터를 보내온 쪽이 "현재 연결"이 되어 마이크 소리도 그쪽으로 나간다.
enum Link { LINK_NONE, LINK_USB, LINK_BLE, LINK_WIFI };

void link_init(void (*set_mic)(bool on));

// PC 가 보낸 JSON 메시지 하나를 넘긴다. 어느 태스크에서 불러도 된다 (처리는 link_loop 에서).
//   {"services": [...], "updated": "12:34"}  사용량 화면 갱신
//   {"mic": true}                            마이크 켜기/끄기
void link_submit(Link from, const char *data, size_t len);

void link_loop();
Link link_active();
