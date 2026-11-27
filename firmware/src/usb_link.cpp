#include "usb_link.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <time.h>

#include <string>

#include "link.h"
#include "mirror.h"
#include "usb_drive.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"

#define MAX_LINE 16384
// PC 에서 받던 것이 이만큼 끊기면 USB 를 다시 붙인다 (도우미는 USB 로 30초마다 보낸다)
#define RX_GAP_MS 120000
// 보낼 것이 쌓였는데 PC 가 이만큼 하나도 가져가지 않으면 USB 를 다시 붙인다.
// Windows 에서 PC→기기는 되는데 기기→PC 가 멈추거나, 둘 다 멈춰 케이블을 다시 꽂아야만 풀린 적이 있다
#define TX_STALL_MS 30000

static std::string line;
static uint32_t rx_ms = 0;

// 회사 Windows PC 에서 USB 가 멈추는 원인을 찾으려고 남기는 것들.
// 멈춘 동안에는 로그가 PC 로 가지 못하므로, 사건은 그때 상태와 함께 NVS 에 저장했다가 PC 가 다시 받을 때 보낸다 ([usb-log])
static uint32_t rx_bytes = 0, short_writes = 0, sof_flips = 0;
static int min_room = INT32_MAX;

struct __attribute__((packed)) UsbEvent {
  uint16_t seq;
  uint32_t epoch;    // 기기 시계 (모르면 0)
  uint32_t up_s;     // 켠 뒤 초
  char why[28];
  uint16_t rx_age_s;  // PC 가 마지막으로 보낸 뒤 (보낸 적 없으면 65535)
  uint16_t room;      // 보내기 버퍼 빈자리
  uint16_t min_room;  // 1분 동안 가장 적었던 빈자리
  uint16_t short_writes, sof_flips;  // 켠 뒤 합계 (잘려 보낸 묶음, USB 신호 끊김·이어짐)
  uint8_t plugged, link, mirror, heap_kb4;  // heap_kb4: 내부 RAM 빈자리 / 4KB
};
#define USB_LOG_MAX 32
static UsbEvent usb_log[USB_LOG_MAX];
static int usb_log_n = 0;
static uint16_t usb_log_seq = 0, usb_log_printed = 0;
static Preferences usb_prefs;

static uint16_t clamp16(uint32_t v) { return v > 65535 ? 65535 : v; }

static void log_event(const char *why) {
  if (usb_log_n == USB_LOG_MAX) memmove(usb_log, usb_log + 1, sizeof(UsbEvent) * --usb_log_n);
  UsbEvent &e = usb_log[usb_log_n++];
  time_t now = time(nullptr);
  e.seq = ++usb_log_seq;
  e.epoch = now > 1700000000 ? (uint32_t)now : 0;
  e.up_s = millis() / 1000;
  strlcpy(e.why, why, sizeof(e.why));
  e.rx_age_s = rx_ms ? clamp16((millis() - rx_ms) / 1000) : 65535;
  e.room = clamp16(max(0, Serial.availableForWrite()));
  e.min_room = clamp16(min_room == INT32_MAX ? 0 : min_room);
  e.short_writes = clamp16(short_writes);
  e.sof_flips = clamp16(sof_flips);
  e.plugged = Serial.isPlugged();
  e.link = (uint8_t)link_active();
  e.mirror = mirror_active();
  e.heap_kb4 = min<size_t>(255, heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 4096);
  usb_prefs.putBytes("events", usb_log, sizeof(UsbEvent) * usb_log_n);
}

// battery.cpp 와 같다: PC 가 받아 가지 않으면 (USB 가 멈춤) 그만두어 화면이 멈추지 않게 한다
static bool send_line(const char *fmt, ...) {
  char buf[200];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  n = min(n, (int)sizeof(buf) - 1);
  return Serial.write((const uint8_t *)buf, n) == (size_t)n;
}

static void print_events() {
  static const char *const links[] = {"none", "usb", "ble", "wifi", "cloud"};
  for (int i = 0; i < usb_log_n; i++) {
    const UsbEvent &e = usb_log[i];
    if ((int16_t)(e.seq - usb_log_printed) <= 0) continue;
    char when[24] = "clock unknown";
    if (e.epoch) {
      time_t t = e.epoch;
      struct tm tm;
      localtime_r(&t, &tm);
      strftime(when, sizeof(when), "%m-%d %H:%M:%S", &tm);
    }
    if (!send_line("[usb-log] #%u %s up %lus: %s | rx age %us, room %u (min %u), short writes %u, sof flips %u, "
                   "plugged %u, link %s, mirror %u, heap %uKB\n",
                   e.seq, when, (unsigned long)e.up_s, e.why, e.rx_age_s, e.room, e.min_room, e.short_writes,
                   e.sof_flips, e.plugged, e.link < 5 ? links[e.link] : "?", e.mirror, e.heap_kb4 * 4))
      return;
    usb_log_printed = e.seq;
  }
}

void usb_link_init() {
  // Serial.begin() 보다 먼저 불러야 한다
  Serial.setRxBufferSize(8192);
  Serial.setTxBufferSize(4096);
  // PC 에서 아무도 읽지 않을 때 로그 출력 때문에 멈추지 않도록 한다
  Serial.setTxTimeoutMs(20);
  usb_prefs.begin("usb-log");
  usb_log_n = usb_prefs.getBytes("events", usb_log, sizeof(usb_log)) / sizeof(UsbEvent);
  if (usb_log_n) usb_log_seq = usb_log[usb_log_n - 1].seq;
  usb_log_printed = usb_log_n ? usb_log[0].seq - 1 : 0;  // 지난 부팅의 것도 모두 보낸다
  // 리셋 이유: 11 = USB (PC 쪽 리셋), 1 = 전원, 3 = 소프트웨어, 4·5·6·7 = 크래시·감시
  char why[28];
  snprintf(why, sizeof(why), "boot, reset reason %d", (int)esp_reset_reason());
  log_event(why);
}

void usb_link_note(const char *note) {
  Serial.printf("[usb] helper note: %s\n", note);
  log_event(note);
}

void usb_reconnect(const char *why) {
  Serial.printf("[usb] reconnecting: %s\n", why);
  log_event(why);
  // D+ 풀업을 잠깐 떼면 PC 는 케이블을 뽑았다 꽂은 것으로 보고 처음부터 다시 연결한다.
  // 기기 재시작(esp_restart)으로는 USB 부분이 초기화되지 않아, PC 를 다시 켤 때 꼬인 USB 가 그대로 남은 적이 있다
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
  delay(200);
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
}

void usb_link_loop() {
  // PC 에서 받은 뒤로 아직 다시 붙이지 않았다
  static bool armed = false;
  // 이번 부팅에서 PC 도우미가 보낸 적이 있다. 다시 붙여도 PC 가 계속 아무것도 보내지 않는데 USB 신호(SOF)는 오가면
  // 간격을 두 배씩 (최대 30분) 늘려 다시 해 본다. 한 번만 하던 때는 도우미가 쓰기 timeout 만 14시간 넘게 되풀이했다
  // (2026-11-14 Mac). 충전기·잠든 PC 는 USB 신호가 없어 하지 않는다. 도우미를 끈 PC 에서는 30분마다 다시 붙는다
  static bool seen_pc = false;
  static uint32_t retry_gap = RX_GAP_MS, retry_ms = 0;
  // 다시 붙인 뒤 PC 가 처음 보내면 남긴다 (얼마 만에 살아났는지)
  static bool waiting_pc = false;
  static uint32_t talk_ms = 0;
  if (Serial.available()) {
    rx_ms = millis() ? millis() : 1;
    if (waiting_pc) {
      waiting_pc = false;
      log_event("PC talking again");
    }
    if (!talk_ms) talk_ms = millis() ? millis() : 1;
    armed = seen_pc = true;
    retry_gap = RX_GAP_MS;
  }
  const char *why = nullptr;
  // USB 드라이브 중에는 USB 가 저장장치 쪽이라 시리얼로 오는 것이 없다. 여기서 다시 붙이면 복사 중인 카드가 끊긴다
  if (usb_drive_active()) {
    armed = seen_pc = false;
  } else if (armed && millis() - rx_ms > RX_GAP_MS) {
    why = "nothing from the PC for 2 minutes";
  } else if (!armed && seen_pc && Serial.isPlugged() && millis() - retry_ms > retry_gap) {
    why = "still nothing from the PC";
    retry_gap = min<uint32_t>(retry_gap * 2, 30 * 60000);
  }
  // 보내기 버퍼의 빈자리가 늘었으면 PC 가 가져간 것이다. 0 은 잠금 대기일 수도 있어 보지 않는다
  static int most_free = 0, last_free = 0;
  static uint32_t moved_ms = 0;
  int room = Serial.availableForWrite();
  if (room > most_free) most_free = room;
  if (room == most_free || (room > 0 && room > last_free) || !armed || !Serial.isPlugged()) moved_ms = millis();
  if (room > 0) last_free = room;
  if (millis() - moved_ms > TX_STALL_MS) why = "the PC stopped taking data for 30 s";
  if (why) {
    armed = false;
    waiting_pc = true;
    talk_ms = 0;
    moved_ms = retry_ms = millis();
    usb_reconnect(why);
  }
  // USB 신호(SOF)가 끊기거나 이어진 횟수, 1분 동안 가장 적었던 보내기 빈자리
  static bool was_plugged = false;
  bool plugged = Serial.isPlugged();
  if (plugged != was_plugged) ++sof_flips;
  was_plugged = plugged;
  if (room > 0 && room < min_room) min_room = room;
  // 1분마다 USB 상태를 남긴다. 멈추기 직전에 무엇이 달라지는지 도우미 기록에서 보려고
  static uint32_t stat_ms = 0;
  if (millis() - stat_ms >= 60000) {
    stat_ms = millis();
    Serial.printf("[usb-stat] rx %lu B, rx age %lus, room %d/%d (min %d), short writes %lu, sof flips %lu, plugged %d, "
                  "armed %d, link %d, mirror %d\n",
                  (unsigned long)rx_bytes, rx_ms ? (unsigned long)((millis() - rx_ms) / 1000) : 0UL, room, most_free,
                  min_room == INT32_MAX ? -1 : min_room, (unsigned long)short_writes, (unsigned long)sof_flips, plugged,
                  armed, (int)link_active(), mirror_active());
    min_room = INT32_MAX;
  }
  // PC 가 10초 동안 계속 보내면 (포트를 열고 읽는 중) 쌓인 USB 사건을 보낸다
  // 보내다 멈추면 30초 뒤에 다시 해 본다 (줄마다 최대 0.4초 기다리므로 매번 하면 화면이 끊긴다)
  static uint32_t print_ms = 0;
  if (talk_ms && millis() - talk_ms > 10000 && usb_log_n && usb_log[usb_log_n - 1].seq != usb_log_printed &&
      (!print_ms || millis() - print_ms > 30000)) {
    print_ms = millis();
    print_events();
  }
  // 입력이 계속 쌓여도 link_loop/UI 가 돌아가야 큐가 비워지고 명령이 실행된다.
  // 긴 JSON 은 다음 loop 에 이어 받는다. 사진/터치 부하 중에도 입력만 읽으며 굶지 않는다.
  unsigned bytes = 0, lines = 0;
  while (Serial.available() && bytes < 2048 && lines < 4) {
    ++bytes;
    ++rx_bytes;
    char c = Serial.read();
    if (c == '\n') {
      ++lines;
      if (!line.empty() && line[0] == '{') {
        String reply;
        if (!link_submit(LINK_USB, line.data(), line.size(), true, &reply))
          usb_send_event("{\"link_error\":\"not_queued\"}");
        else if (reply.length()) usb_send_event(reply.c_str());
      }
      line.clear();
    } else if (line.size() < MAX_LINE) {
      line += c;
    }
  }
}

void usb_send_audio(uint32_t seq, const int16_t *pcm, size_t samples) {
  // 로그 출력과 섞이지 않도록 프레임 전체를 한 번에 쓴다
  static uint8_t frame[5 + 4 + 320 * 2];
  size_t body = 4 + samples * 2;
  if (5 + body > sizeof(frame)) return;
  frame[0] = 0xA5;
  frame[1] = 0x5A;
  frame[2] = USB_FRAME_AUDIO;
  frame[3] = body & 0xFF;
  frame[4] = body >> 8;
  memcpy(frame + 5, &seq, 4);
  memcpy(frame + 9, pcm, samples * 2);
  if (Serial.write(frame, 5 + body) != 5 + body) ++short_writes;
}

bool usb_send_event(const char *json) {
  // 거의 다 짧다. 여러 태스크가 부르므로 스택은 그대로 두고 긴 것 (위치 조정 결과) 만 PSRAM 에 잡는다
  uint8_t small[5 + 200];
  uint8_t *frame = small;
  size_t body = strlen(json);
  if (body > 2048) return false;  // companion/transports/frames.py 의 MAX_FRAME
  if (5 + body > sizeof(small)) {
    frame = (uint8_t *)ps_malloc(5 + body);
    if (!frame) return false;
  }
  frame[0] = 0xA5;
  frame[1] = 0x5A;
  frame[2] = USB_FRAME_EVENT;
  frame[3] = body & 0xFF;
  frame[4] = body >> 8;
  memcpy(frame + 5, json, body);
  bool sent = Serial.write(frame, 5 + body) == 5 + body;
  if (!sent) ++short_writes;
  if (frame != small) free(frame);
  return sent;
}

void usb_send_photo_chunk(uint32_t offset, const uint8_t *data, size_t len) {
  static uint8_t frame[5 + 4 + 1024];
  size_t body = 4 + len;
  if (5 + body > sizeof(frame)) return;
  frame[0] = 0xA5;
  frame[1] = 0x5A;
  frame[2] = USB_FRAME_PHOTO;
  frame[3] = body & 0xFF;
  frame[4] = body >> 8;
  memcpy(frame + 5, &offset, 4);
  memcpy(frame + 9, data, len);
  if (Serial.write(frame, 5 + body) != 5 + body) ++short_writes;
}

void usb_send_frame(uint8_t kind, const uint8_t *head, size_t head_len, const uint8_t *data, size_t len) {
  // 여러 태스크가 보내므로 프레임마다 버퍼를 따로 쓴다
  uint8_t frame[5 + 2048];
  size_t body = head_len + len;
  if (5 + body > sizeof(frame)) return;
  frame[0] = 0xA5;
  frame[1] = 0x5A;
  frame[2] = kind;
  frame[3] = body & 0xFF;
  frame[4] = body >> 8;
  if (head_len) memcpy(frame + 5, head, head_len);
  if (len) memcpy(frame + 5 + head_len, data, len);
  if (Serial.write(frame, 5 + body) != 5 + body) ++short_writes;
}

bool usb_host_present() {
  static uint32_t lost_ms = 0;
  if (Serial.isPlugged()) {
    lost_ms = 0;
    return true;
  }
  if (!lost_ms) lost_ms = millis() ? millis() : 1;
  return millis() - lost_ms < 3000;
}

uint32_t usb_last_rx_ms() {
  return rx_ms;
}
