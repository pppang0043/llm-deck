#pragma once

// USB 드라이브 모드 (빠른 설정 > USB 드라이브): PC 가 SD 카드를 USB 저장장치로 연다.
// 켜면 USB 가 평소의 시리얼(PC 도우미 연결·로그)에서 저장장치로 바뀌고, 기기는 카드 파일을 건드리지 않는다.
// PC 도우미는 그동안 블루투스·Wi-Fi 로 이어진다. 끝낼 때는 기기를 다시 시작한다 (USB 가 시리얼로 돌아온다).

// setup() 맨 앞에서 부른다. 지난번에 USB 드라이브 모드였으면 USB 를 시리얼로 되돌리고 true.
// (그때는 Serial.begin() 뒤에 usb_reconnect 로 PC 가 다시 알아보게 한다)
bool usb_drive_boot();
// 켠다. 켜지 못하면 그 이유 (카메라·갤러리가 열려 있음, 카드 없음), 켰으면 nullptr.
const char *usb_drive_start();
bool usb_drive_active();
// loop() 에서 부른다 (화면의 연결 상태)
void usb_drive_loop();
