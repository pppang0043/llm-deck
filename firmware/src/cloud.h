#pragma once

// PC 가 연결되어 있지 않을 때, 기기가 Wi-Fi 로 직접 공유 저장소(GitHub gist)에서 한도 사용률을 읽어 온다.
// 주소는 PC 도우미가 한 번 알려주면 기기가 기억한다. 읽기만 하므로 로그인 정보는 필요 없다.
void cloud_init();

// PC 도우미가 알려준 공유 주소를 저장한다.
void cloud_set_url(const char *url);
