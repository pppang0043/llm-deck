#pragma once

#include <Arduino.h>

// 보드의 microSD 카드. 필요할 때 연결(마운트)하고, 빠지면 다음에 다시 시도한다.
bool storage_ready();
// 파일을 쓴다. path 는 카드 안의 경로 (예: "/photos/IMG_1.jpg"). 폴더는 알아서 만든다.
bool storage_write(const char *path, const uint8_t *data, size_t len);
