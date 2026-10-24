#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// HTTP Content-Length 가 없거나 거짓이어도 실제로 받은 크기를 제한한다.
class BoundedBuffer {
 public:
  BoundedBuffer(uint8_t *data, size_t capacity) : data_(data), capacity_(capacity) {}
  size_t append(const uint8_t *data, size_t len) {
    if (failed_ || !data_ || len > capacity_ - size_) { failed_ = true; return 0; }
    memcpy(data_ + size_, data, len);
    size_ += len;
    return len;
  }
  size_t size() const { return size_; }
  bool failed() const { return failed_; }
 private:
  uint8_t *data_;
  size_t capacity_, size_ = 0;
  bool failed_ = false;
};
