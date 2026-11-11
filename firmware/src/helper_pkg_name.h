#pragma once

#include <stddef.h>
#include <string.h>

// 카드에 만들 파일 이름: 영문·숫자·. _ - 만, 점으로 시작하지 않고 40자 안
inline bool helper_pkg_name_ok(const char *name) {
  size_t n = name ? strlen(name) : 0;
  if (n == 0 || n > 40 || name[0] == '.') return false;
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return false;
  }
  return true;
}
