#pragma once
#include <cstdio>
#include <cstring>

// 두 화면이 공유하는 미확인 완료 알림. 시간 경과는 읽음이 아니며, 열람만 소진한다.
class CompletionInbox {
 public:
  static constexpr unsigned CAPACITY = 8;
  static constexpr unsigned TEXT_SIZE = 280;  // "ORCA-프로젝트(20자)-세션(30자)\n요약(40자)" 전부 한글이어도 들어간다
  static constexpr unsigned SNAPSHOT_SIZE = CAPACITY * (TEXT_SIZE + 2) + 1;
  void push(const char *text) {
    if (!text || !*text) return;
    if (count_ == CAPACITY) {
      std::memmove(items_, items_ + 1, sizeof(items_[0]) * (CAPACITY - 1));
      --count_;
    }
    unsigned n = std::strlen(text);
    if (n >= TEXT_SIZE) {
      n = TEXT_SIZE - 1;
      while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;
    }
    std::memcpy(items_[count_], text, n);
    items_[count_++][n] = 0;
  }
  unsigned count() const { return count_; }
  const char *latest() const { return count_ ? items_[count_ - 1] : ""; }
  // 읽는 화면에 스냅샷을 전달하고 즉시 공유 목록에서 지운다. 다른 화면/다음 열람에는 재등장하지 않는다.
  void take(char *out, unsigned size) {
    if (!size) return;
    out[0] = 0;
    unsigned used = 0;
    for (unsigned i = count_; i && used < size - 1; --i) {
      int n = std::snprintf(out + used, size - used, "%s%s", used ? "\n\n" : "", items_[i - 1]);
      if (n < 0 || static_cast<unsigned>(n) >= size - used) break;
      used += n;
    }
    count_ = 0;
  }
 private:
  char items_[CAPACITY][TEXT_SIZE] = {};
  unsigned count_ = 0;
};
