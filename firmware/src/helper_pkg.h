#pragma once

#include <ArduinoJson.h>
#include "helper_pkg_name.h"

// PC 도우미 설치 파일을 SD 카드의 /LLM-Deck-Helper/ 에 받는다 (도우미 companion/sdpkg.py 가 USB 로 보낸다).
// 새 PC 에서 빠른 설정 > USB 드라이브 로 카드를 열면 거기서 바로 설치한다.
//   PC -> 기기: {"pkg": {"ask": true}}                         -> {"pkg": {"card": bool, "ver": 카드의 version.txt}}
//               {"pkg": {"name", "off", "data": base64, "done"}} -> {"pkg": {"name", "off": 받아 둔 크기}} (오류는 "error")
// name.part 에 이어 쓰고 done 이면 name 으로 바꾼다. off 가 받아 둔 크기와 다르면 쓰지 않고 받아 둔 크기를 알린다.
// USB 로 온 것만 처리한다 (link.cpp). 카드는 화면과 같은 SPI 라 loop 에서만 부른다.
#define HELPER_PKG_DIR "/LLM-Deck-Helper"
#define HELPER_PKG_CHUNK 6144  // 한 번에 받는 원래 바이트 (base64 로 8KB, USB 한 줄 16KB 안)

void helper_pkg_command(JsonObjectConst pkg);
