#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// 잠금은 호출자가 맡는다. 명령과 최신 상태는 각각 8칸을 사용한다.
// 이미 받은 명령을 밀어내지 않으며, 할당 실패도 재시작 대신 거절한다.
class MessageQueue {
 public:
  static constexpr size_t MAX_MESSAGE = 16384;
  static constexpr size_t MAX_BYTES = 65536;
  struct Item {
    char *data = nullptr;
    size_t len = 0;
    int from = 0;
    bool usage = true;
    uint32_t snapshot = 0;
    int command_ticket = -1;
    uint64_t peer = 0;
  };
  explicit MessageQueue(void *(*allocate)(size_t) = malloc, void (*release)(void *) = free)
      : allocate_(allocate), release_(release) {}
  ~MessageQueue() { for (size_t i = 0; i < count_; ++i) release_(items_[i].data); }
  MessageQueue(const MessageQueue &) = delete;
  MessageQueue &operator=(const MessageQueue &) = delete;

  bool push(int from, bool usage, uint32_t snapshot, const char *data, size_t len, int command_ticket = -1, uint64_t peer = 0) {
    if (!data || !len || len > MAX_MESSAGE) return false;
    size_t replace = count_, same_kind = 0, kind_bytes = 0;
    for (size_t i = 0; i < count_; ++i) {
      const Item &p = items_[i];
      if (bool(p.snapshot) == bool(snapshot)) { ++same_kind; kind_bytes += p.len + 1; }
      if (!p.snapshot) replace = count_;  // 명령 앞의 상태는 그 명령이 볼 수 있게 보존
      if (snapshot && p.snapshot == snapshot && p.from == from && p.usage == usage && p.peer == peer) replace = i;
    }
    size_t old_size = replace < count_ ? items_[replace].len + 1 : 0;
    if ((replace == count_ && same_kind >= 8) || kind_bytes - old_size + len + 1 > MAX_BYTES / 2) return false;
    char *copy = static_cast<char *>(allocate_(len + 1));
    if (!copy) return false;  // 이전 값도 그대로 보존한다
    memcpy(copy, data, len);
    copy[len] = 0;
    if (replace < count_) {
      release_(items_[replace].data);
      erase(replace);
      bytes_ -= old_size;
    }
    items_[count_++] = {copy, len, from, usage, snapshot, command_ticket, peer};
    bytes_ += len + 1;
    return true;
  }
  bool pop(Item &out) {
    if (!count_) return false;
    out = items_[0];
    bytes_ -= out.len + 1;
    erase(0);
    return true;  // out.data 는 호출자가 release 해야 한다
  }

 private:
  void erase(size_t at) {
    for (size_t i = at + 1; i < count_; ++i) items_[i - 1] = items_[i];
    --count_;
  }
  Item items_[16];
  size_t count_ = 0, bytes_ = 0;
  void *(*allocate_)(size_t);
  void (*release_)(void *);
};
