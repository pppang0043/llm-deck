#pragma once

#include <Arduino.h>

// PC 와 연결하는 세 가지 방법. 셋 다 항상 켜져 있고,
// 가장 최근에 데이터를 보내온 쪽이 "현재 연결"이 되어 마이크 소리도 그쪽으로 나간다.
// LINK_CLOUD 는 PC 없이 기기가 직접 공유 저장소에서 읽어 온 경우다 (cloud.h).
enum Link { LINK_NONE, LINK_USB, LINK_BLE, LINK_WIFI, LINK_CLOUD };

void link_init(void (*set_mic)(bool on));

// PC 가 보낸 JSON 메시지 하나를 넘긴다. 어느 태스크에서 불러도 된다 (처리는 link_loop 에서).
//   {"services": [...], "updated": "12:34"}  사용량 화면 갱신
//   {"now": 1700000000, "utc_offset": 32400} 현재 시각(UTC 초)과 시간대(초). 기기 시계를 맞춘다
//   {"mic": true}                            마이크 켜기/끄기
//   {"sync_url": "https://..."}              공유 저장소 주소 (기기가 기억한다)
//   {"wifi_add": {"ssid", "password"}}       Wi-Fi 추가 등록 / {"wifi_clear": true} 추가한 것 모두 삭제
void link_submit(Link from, const char *data, size_t len);

void link_loop();
Link link_active();
