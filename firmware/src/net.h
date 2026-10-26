#pragma once

// Wi-Fi 로 PC 와 통신한다. PC 도우미가 HTTP 로 데이터를 보내고, 마이크 소리는 UDP 로 나간다.
// 등록된 Wi-Fi 중 잡히는 곳에 접속한다. 등록된 곳이 없으면 Wi-Fi 를 쓰지 않는다.
//   - secrets.h 에 적힌 Wi-Fi 하나
//   - PC 도우미로 추가한 Wi-Fi (기기에 저장, 최대 NET_MAX_SAVED 개)
#define NET_MAX_SAVED 5

void net_init();
void net_loop();

// Wi-Fi 신호 칸 수 0~4 (상태줄 아이콘, 미러링 속도에 쓴다). Wi-Fi 에 붙어 있지 않으면 -1
int net_wifi_bars();

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

// Wi-Fi 인증. 같은 네트워크의 누구나 기기에 명령을 보내거나 PC 인 척할 수 없게, USB 로 한 번 받아 간
// 비밀 키(32바이트)로 서명한다. 키는 기기가 처음 요청받을 때 만들어 기기(NVS)에 저장한다.
//   PC -> 기기 HTTP: 머리글 X-Deck-Auth: <nonce 32자>.<순번>.<서명 64자>
//       서명 = HMAC-SHA256(키, "http" + nonce 16바이트 + 순번 4바이트(LE) + 경로 + "\n" + 내용)
//       nonce 는 /status 가 줄 때마다 새로 만든다 (기기가 다시 켜지면 옛 것은 모두 무효). 같은 순번은 한 번만 받는다
//   기기 -> PC UDP: [내용][순번 4바이트(LE)][서명 앞 16바이트]
//       서명 = HMAC-SHA256(키, "udp" + 그 PC 의 nonce 16바이트 + 순번 4바이트 + 내용)
//   기기 -> PC TCP: 접속하면 PC 가 16바이트 도전값을 보내고, 기기는 첫 묶음 [A5 5A][6][32][HMAC(키, "tcp" + 도전값)] 으로 답한다
#define NET_FRAME_AUTH 6
#define NET_UDP_TRAILER 20
// USB 로 연결한 PC 에 줄 {"pair": {"id": 기기 MAC, "key": 키 64자}} (키가 없으면 만든다). 블루투스는 암호화가 없어 주지 않는다
String net_pair_json();
// 블루투스 인증 (ble_link.h) 에 같은 키로 서명한다: HMAC-SHA256(키, label + a + b + c). 키가 없으면 false
bool net_ble_mac(const char *label, const void *a, size_t an, const void *b, size_t bn, const void *c, size_t cn,
                 uint8_t out[32]);
// PC 가 기기별 키를 고르는 이름표 (Wi-Fi MAC 12자)
String net_device_id();
// 인증된 Wi-Fi PC 중 현재 선택된 주소인지 (조회는 소유권을 갱신하지 않는다).
bool net_peer_is_owner(uint32_t peer);
// UDP 로 보낼 내용 뒤에 붙일 순번·서명 (NET_UDP_TRAILER 바이트). Wi-Fi 로 인증된 PC 가 없으면 false. 어느 태스크에서 불러도 된다
bool net_udp_trailer(const uint8_t *data, size_t len, uint8_t *trailer);
// TCP 로 PC 에 접속한 직후 부른다 (위 TCP 인증). 실패하면 false (끊고 다시 시도)
#include <WiFiClient.h>
bool net_tcp_hello(WiFiClient &client);
