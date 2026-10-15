#pragma once

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// ArduinoJson 이 내부 RAM 대신 PSRAM 을 쓰게 하는 할당기.
// 사용량 데이터를 30초마다 받으며 잡았다 놓기를 반복하면 내부 RAM 이 잘게 쪼개져
// 카메라처럼 큰 연속 덩어리가 필요한 기능이 켜지지 않는다.
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  }
  void deallocate(void *ptr) override {
    heap_caps_free(ptr);
  }
  void *reallocate(void *ptr, size_t size) override {
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
  }
  static PsramAllocator *instance() {
    static PsramAllocator allocator;
    return &allocator;
  }
};

// JsonDocument doc(PSRAM_JSON);
#define PSRAM_JSON PsramAllocator::instance()
