#pragma once

// Wi-Fi 로 PC 와 통신한다. PC 도우미가 HTTP 로 데이터를 보내고, 마이크 소리는 UDP 로 나간다.
// 등록된 Wi-Fi 중 잡히는 곳에 접속한다. 등록된 곳이 없으면 Wi-Fi 를 쓰지 않는다.
//   - secrets.h 에 적힌 Wi-Fi 하나
//   - PC 도우미로 추가한 Wi-Fi (기기에 저장, 최대 NET_MAX_SAVED 개)
#define NET_MAX_SAVED 5

void net_init();
void net_loop();

// Wi-Fi 를 추가로 등록한다. 같은 이름이 있으면 비밀번호를 바꾼다.
void net_add_wifi(const char *ssid, const char *password);

// PC 도우미로 추가한 Wi-Fi 를 모두 지운다 (secrets.h 의 것은 남는다).
void net_clear_wifi();
// Wi-Fi 를 켜고 끈다 (설정에서 Wi-Fi 와 클라우드를 모두 끄면 끈다)
void net_set_enabled(bool on);
// 등록된 Wi-Fi 이름들 (줄바꿈으로 구분, secrets.h 의 것이 먼저). 설정 화면에 보여준다.
#include <Arduino.h>
String net_saved_names();
// PC 도우미로 추가한 Wi-Fi 개수 (secrets.h 의 것은 빼고)
int net_added_count();
