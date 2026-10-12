#pragma once

// Wi-Fi 로 PC 와 통신한다. PC 도우미가 HTTP 로 데이터를 보내고, 마이크 소리는 UDP 로 나간다.
// secrets.h 에 Wi-Fi 정보가 있으면 그 공유기에 접속하고,
// 비어 있으면 기기가 직접 Wi-Fi(AP_SSID)를 만들어 PC 가 거기에 접속할 수 있게 한다.
void net_init();
void net_loop();
