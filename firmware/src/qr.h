#pragma once

#include <Arduino.h>

// 기기에서 바로 QR 코드를 읽는다 (quirc, lib/quirc). PC 가 없어도 된다.
// JPEG 한 장을 흑백으로 풀어 찾는다. 1024x768 한 장에 0.5초 안팎이라 화면 루프가 아니라 카메라 작업 태스크에서 부른다
// (스택 12KB 넘게 쓴다). 버퍼는 모두 PSRAM (1024x768 이면 잠깐 2.4MB).
// 찾으면 읽은 글(UTF-8, 끝 0)을 ps_malloc 해 돌려준다 (free 로 해제). 없거나 못 읽으면 nullptr
char *qr_decode_jpeg(const uint8_t *jpg, size_t len);
