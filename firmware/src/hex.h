#pragma once

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// 서명·nonce 를 글자로 주고받을 때 쓴다 (net.cpp, ble_link.cpp)
inline void to_hex(const uint8_t *data, size_t len, char *out) {
  for (size_t i = 0; i < len; i++) sprintf(out + i * 2, "%02x", data[i]);
}

inline bool from_hex(const char *text, size_t len, uint8_t *out) {
  for (size_t i = 0; i < len; i++) {
    unsigned v;
    if (!isxdigit((unsigned char)text[i * 2]) || !isxdigit((unsigned char)text[i * 2 + 1])) return false;
    sscanf(text + i * 2, "%2x", &v);
    out[i] = v;
  }
  return true;
}
