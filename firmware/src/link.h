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
//   {"battery_mv": 4123}                     멀티미터로 잰 배터리 전압. 기기의 전압 측정을 보정한다 (0: 보정 지움)
//   {"wifi_add": {"ssid", "password"}}       Wi-Fi 추가 등록 / {"wifi_clear": true} 추가한 것 모두 삭제
void link_submit(Link from, const char *data, size_t len);

void link_loop();

// 현재 연결된 PC 로 알림을 보낸다 (JSON 글자, 짧게). PC 가 없으면 버린다.
//   {"sound": "mic_on"}   PC 스피커로 소리를 내 달라는 요청 (mic_on / mic_off). 소리는 PC 도우미가 고른다
// Wi-Fi 에서는 마이크 소리와 같은 UDP 포트로 간다 (길이가 달라서 구별된다).
void link_send_event(const char *json);
Link link_active();
