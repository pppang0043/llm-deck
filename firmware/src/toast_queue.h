#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// 화면 태스크(loop) 밖에서 부른 알림을 loop 가 꺼낼 때까지 맡아 둔다. 잠금은 호출자가 맡는다.
// 가득 차면 새 것을 버린다. 태스크 스택이나 임시 문자열에서 온 글일 수 있어 내용을 복사해 둔다.
class ToastQueue {
 public:
  static constexpr size_t SLOTS = 4;
  static constexpr size_t TEXT = 120;

  bool push(const char *text, uint32_t ms) {
    if (count_ == SLOTS) return false;
    Item &item = items_[(head_ + count_) % SLOTS];
    size_t n = strlen(text);
    if (n >= TEXT) {
      // 한글(UTF-8 여러 바이트) 글자 중간에서 끊기지 않게 글자 앞까지 물린다
      n = TEXT - 1;
      while (n > 0 && ((uint8_t)text[n] & 0xC0) == 0x80) n--;
    }
    memcpy(item.text, text, n);
    item.text[n] = '\0';
    item.ms = ms;
    count_++;
    return true;
  }

  // text 는 TEXT 바이트 이상이어야 한다
  bool pop(char *text, uint32_t *ms) {
    if (!count_) return false;
    const Item &item = items_[head_];
    strcpy(text, item.text);
    *ms = item.ms;
    head_ = (head_ + 1) % SLOTS;
    count_--;
    return true;
  }

 private:
  struct Item {
    char text[TEXT];
    uint32_t ms;
  };
  Item items_[SLOTS];
  size_t head_ = 0, count_ = 0;
};
