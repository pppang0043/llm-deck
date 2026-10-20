#pragma once

#include <Arduino.h>

// PC 와 연결하는 세 가지 방법. 셋 다 항상 켜져 있고,
// 데이터를 보내오는 쪽 중 우선순위가 가장 높은 쪽(USB > Wi-Fi > 블루투스)이 "현재 연결"이 되어
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
//   {"bt_check": true}                      블루투스 스피커 시험: 칩 지원 범위 + 주변 기기 찾기 (결과는 시리얼 [bt-check])
//   {"bt_check": "aa:bb:cc:dd:ee:ff"}       위 + 그 기기에 연결해 서비스 목록 읽기 ("all": 연결 가능한 기기 모두)
//   {"ask": {"state", "q", "a", "done"}}     질문 화면: 받아 적은 질문과 나오는 중인 답 (askpanel.h)
//   {"ask_mic": false}                       PC 에 쓸 수 있는 마이크가 없음 (말하기 버튼을 끈다)
//   {"host": "PC 이름"}                      보낸 PC 의 이름 (블루투스 설정 화면에 보여준다)
//   {"helper": {"proto", "ver", "at", "err"}} 도우미 버전과 마지막 사용량 수집 시각·오류 (설정 > 정보)
//   {"ask_fw": true}                        펌웨어 버전({"fw_proto"})을 다시 알려 달라 (사용량과 함께, 도우미가 모를 때)
// usage 가 false 면 사용량("services")은 쓰지 않는다 (블루투스로 붙어 기다리는 중인 PC)
void link_submit(Link from, const char *data, size_t len, bool usage = true);

void link_loop();

// 현재 연결된 PC 로 알림을 보낸다 (JSON 글자, 짧게). PC 가 없으면 버린다.
//   {"sound": "mic_on"}   PC 스피커로 소리를 내 달라는 요청 (mic_on / mic_off). 소리는 PC 도우미가 고른다
// Wi-Fi 에서는 마이크 소리와 같은 UDP 포트로 간다 (길이가 달라서 구별된다).
void link_send_event(const char *json);
Link link_active();
// {"rotate": true} 를 받으면 부를 함수 (화면 돌리기 버튼과 같다)
void link_set_rotate(void (*rotate)());

// PC 도우미와 주고받는 내용의 버전. 메시지 칸·인증 방식이 바뀔 때 올린다 (companion/version.py 의 PROTO 와 같아야 한다).
// 사용량을 받으면 연결마다 한 번 {"fw_proto": N, "fw": "빌드 시각"} 으로 알려 주고, 다르면 알림을 띄운다.
#define FW_PROTO 3

// 지금 쓰는 PC 도우미가 알려 준 것 (사용량 메시지의 "helper"). 보내지 않는 옛 도우미는 proto 0
struct HelperInfo {
  bool known;      // 사용량을 한 번이라도 받았는지
  int proto;
  char host[32];
  char ver[24];
  int64_t at;      // 마지막 수집 시각 (UTC 초, 0: 모름)
  char err[72];    // 마지막 수집 오류 한 줄 ("" 이면 없음)
};
const HelperInfo &link_helper();
