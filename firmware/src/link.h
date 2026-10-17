#pragma once

#include <Arduino.h>

// PC 와 연결하는 세 가지 방법. 셋 다 항상 켜져 있고,
// 데이터를 보내오는 쪽 중 우선순위가 가장 높은 쪽(USB > 블루투스 > Wi-Fi)이 "현재 연결"이 되어
// 마이크 소리·사진·알림도 그쪽으로 나간다.
// LINK_CLOUD 는 PC 없이 기기가 직접 공유 저장소에서 읽어 온 경우다 (cloud.h).
enum Link { LINK_NONE, LINK_USB, LINK_BLE, LINK_WIFI, LINK_CLOUD };

void link_init(void (*set_mic)(bool on));

// PC 가 보낸 JSON 메시지 하나를 넘긴다. 어느 태스크에서 불러도 된다 (처리는 link_loop 에서).
//   {"services": [...], "updated": "12:34"}  사용량 화면 갱신
//   {"now": 1700000000, "utc_offset": 32400} 현재 시각(UTC 초)과 시간대(초). 기기 시계를 맞춘다
//   {"mic": true}                            마이크 켜기/끄기
//   {"sync_url": "https://..."}              공유 저장소 주소 (기기가 기억한다)
//   {"camera": "open"}                     카메라 화면 열기 / "shoot" 촬영 / "record" 녹화 시작·멈춤 / "close" 닫기
//   {"camera_mount": {"rot": 1, "mirror": false}}  카메라 센서가 붙은 방향 (사진이 돌아가 보이면 맞춘다)
//   {"view": "home"}                       화면 바꾸기 (home / usage / info)
//   {"screenshot": true}                    지금 화면을 사진처럼 PC 로 보낸다 (진단용)
//   {"links": {"ble": false}}              연결 방식 켜고 끄기 (usb/ble/wifi/cloud). USB 로 온 것만, USB 를 꺼도 받는다
//   {"photo_test": true}                    카메라 없이 무늬 사진으로 저장·전송을 시험한다
//   {"battery_mv": 4123}                     멀티미터로 잰 배터리 전압. 기기의 전압 측정을 보정한다 (0: 보정 지움)
//   {"wifi_add": {"ssid", "password"}}       Wi-Fi 추가 등록 / {"wifi_clear": true} 추가한 것 모두 삭제
//   {"host": "PC 이름"}                      보낸 PC 의 이름 (블루투스 설정 화면에 보여준다)
// usage 가 false 면 사용량("services")은 쓰지 않는다 (블루투스로 붙어 기다리는 중인 PC)
void link_submit(Link from, const char *data, size_t len, bool usage = true);

void link_loop();

// 현재 연결된 PC 로 알림을 보낸다 (JSON 글자, 짧게). PC 가 없으면 버린다.
//   {"sound": "mic_on"}   PC 스피커로 소리를 내 달라는 요청 (mic_on / mic_off). 소리는 PC 도우미가 고른다
// Wi-Fi 에서는 마이크 소리와 같은 UDP 포트로 간다 (길이가 달라서 구별된다).
void link_send_event(const char *json);
Link link_active();
