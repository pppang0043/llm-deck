#pragma once

// Wi-Fi 로 PC 와 통신한다. PC 도우미가 HTTP 로 데이터를 보내고, 마이크 소리는 UDP 로 나간다.
// secrets.h 에 적힌 공유기에 접속한다. 비어 있으면 Wi-Fi 를 쓰지 않는다.
void net_init();
void net_loop();
