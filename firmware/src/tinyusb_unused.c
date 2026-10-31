// USB 드라이브(usb_drive.cpp)는 TinyUSB 의 저장장치 기능만 쓰는데, 미리 만든 라이브러리는 usbd.c·usbh.c 가
// 다른 기능을 모두 불러 네트워크·오디오·DFU·USB 호스트의 버퍼까지 내부 RAM 에 약 21KB 를 잡는다.
// 여기서 그 진입점을 빈 함수로 두면 링커가 해당 코드와 버퍼를 넣지 않는다.
// 장치 쪽은 usbd 가 기능마다 init 을 부르고 open 이 0 을 돌려주면 다음 기능으로 넘어간다. 호스트 쪽은 부르지 않는다.

#include <stdbool.h>
#include <stdint.h>

#define DEVICE_CLASS(name)                                       \
  void name##_init(void) {}                                      \
  bool name##_deinit(void) { return true; }                      \
  void name##_reset(uint8_t rhport) {}                           \
  uint16_t name##_open(void) { return 0; }                       \
  bool name##_control_xfer_cb(void) { return false; }

#define HOST_CLASS(name)                       \
  bool name##_init(void) { return false; }     \
  bool name##_deinit(void) { return true; }    \
  bool name##_open(void) { return false; }     \
  bool name##_set_config(void) { return false; } \
  bool name##_xfer_cb(void) { return false; }  \
  void name##_close(void) {}

DEVICE_CLASS(netd)
bool netd_xfer_cb(void) { return false; }

DEVICE_CLASS(audiod)
bool audiod_xfer_cb(void) { return false; }
bool audiod_xfer_isr(void) { return false; }
void audiod_sof_isr(void) {}

DEVICE_CLASS(dfu_moded)

HOST_CLASS(hidh)
HOST_CLASS(msch)
HOST_CLASS(cdch)
HOST_CLASS(midih)
