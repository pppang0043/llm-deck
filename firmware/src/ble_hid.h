#pragma once

#include <Arduino.h>

// 블루투스 키보드 모드: 기기가 블루투스 키보드(와 소리 크기 키)가 되어 PC 도우미 없이 아무 PC·휴대폰에나 키를 보낸다
// (단축키 화면의 "리모컨"). 설정 > 블루투스 에서 켜며, 바꾸면 다시 시작한다.
// 켜져 있으면 블루투스는 키보드로만 쓴다 (PC 도우미의 블루투스 연결은 쉰다. USB·Wi-Fi 연결은 그대로).
// 키보드는 페어링(암호화)이 필요해, 도우미 연결(앱에서 인증)과 한꺼번에 켜면 도우미 연결이 깨질 수 있어 나눴다.
bool ble_hid_mode();
void ble_hid_set_mode(bool on);  // 저장하고 다시 시작한다
void ble_hid_init();
bool ble_hid_connected();
// 키 하나를 눌렀다 뗀다 (HID 키 코드, 예: 0x28 Enter). 연결이 없으면 false
bool ble_hid_key(uint8_t keycode, uint8_t modifiers = 0);
// 소리 크기 같은 미디어 키 (Consumer 페이지 코드, 예: 0xE9 소리 크게)
bool ble_hid_media(uint16_t usage);
