#include "toast_queue.h"
#include <assert.h>
#include <string>

int main() {
  char text[ToastQueue::TEXT];
  uint32_t ms = 0;
  {
    ToastQueue q;
    assert(!q.pop(text, &ms));
    // 넣은 순서대로 나온다. 스택의 글이 사라져도 복사해 둔 내용이 나온다.
    {
      char temp[8] = "first";
      assert(q.push(temp, 100));
      temp[0] = 'X';
    }
    assert(q.push("second", 200));
    assert(q.pop(text, &ms) && std::string(text) == "first" && ms == 100);
    assert(q.pop(text, &ms) && std::string(text) == "second" && ms == 200);
    assert(!q.pop(text, &ms));
  }
  {
    // 가득 차면 새 것을 버리고 앞의 것은 그대로다. 비우면 다시 받는다 (끝을 넘어 돌아가도).
    ToastQueue q;
    for (size_t i = 0; i < ToastQueue::SLOTS; ++i) assert(q.push(std::to_string(i).c_str(), i));
    assert(!q.push("dropped", 99));
    for (size_t i = 0; i < ToastQueue::SLOTS; ++i) {
      assert(q.pop(text, &ms) && std::to_string(i) == text && ms == i);
    }
    for (int round = 0; round < 10; ++round) {
      assert(q.push("again", 1));
      assert(q.pop(text, &ms) && std::string(text) == "again");
    }
  }
  {
    // 칸보다 길면 글자 경계에서 자른다 ("가" 는 3바이트)
    ToastQueue q;
    std::string longer;
    while (longer.size() < ToastQueue::TEXT + 30) longer += "가";
    assert(q.push(longer.c_str(), 1));
    assert(q.pop(text, &ms));
    std::string got(text);
    assert(got.size() < ToastQueue::TEXT);
    assert(got.size() % 3 == 0);
    assert(longer.compare(0, got.size(), got) == 0);

    // 딱 칸에 맞는 길이(끝의 0 포함)는 자르지 않는다
    std::string fit(ToastQueue::TEXT - 1, 'a');
    assert(q.push(fit.c_str(), 1));
    assert(q.pop(text, &ms) && fit == text);
  }
  return 0;
}
