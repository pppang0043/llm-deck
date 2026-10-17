// 블루투스(NimBLE) 호스트의 버퍼를 큰 메모리(PSRAM)에서 받게 한다.
// Arduino 의 미리 만든 설정이 내부 RAM 으로 정해 두어서, 링커로 할당 함수를 바꿔 끼운다
// (platformio.ini 의 -Wl,--wrap=nimble_platform_mem_malloc / calloc).
// PSRAM 이 모자라면 원래대로 내부 RAM 에서 받는다.

#include <esp_heap_caps.h>
#include <stddef.h>

void *__real_nimble_platform_mem_malloc(size_t size);
void *__real_nimble_platform_mem_calloc(size_t n, size_t size);

void *__wrap_nimble_platform_mem_malloc(size_t size) {
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : __real_nimble_platform_mem_malloc(size);
}

void *__wrap_nimble_platform_mem_calloc(size_t n, size_t size) {
  void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : __real_nimble_platform_mem_calloc(n, size);
}
