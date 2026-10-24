#include "mirror.h"

#include <WiFi.h>
#include <lvgl.h>
#include <time.h>

#include "display.h"
#include "link.h"
#include "mic.h"
#include "net.h"
#include "orcapanel.h"
#include "pcpanel.h"
#include "ui.h"
#include "usb_link.h"
#include "video.h"

// 창이 이만큼 아무 말이 없으면 (닫혔거나 도우미가 꺼짐) 끈다
#define KEEP_MS 6000
// 장 사이의 최소 간격 (초당 최대 약 25장). 화면 복사가 화면 반응을 늦추지 않게 둔다.
#define MIN_GAP_MS 40
#define CHUNK 2000
#define MAX_PIXELS (320 * 240)
#define DEVICE_STATUS_H 32

LV_FONT_DECLARE(font_ko_14);

struct __attribute__((packed)) Head {
  uint32_t no, offset, total;
  uint16_t w, h;
};

static uint16_t *grab = nullptr;  // 보낼 화면 (PSRAM)
static uint8_t *packed = nullptr;  // 줄인 화면 (PSRAM)
static int grab_w, grab_h;
static volatile bool busy = false;  // 줄이기·보내는 중
static uint32_t until_ms = 0;
// 나눠 쓰기: 켜면 기기 LCD 에는 사용량을 따로 보여 주고, PC 창은 원래 화면을 혼자 쓴다.
// 미러링이 시작될 때마다 켜진 채로 시작하고, 구독제 화면의 작은 "합치기"/"분리" 버튼으로 켜고 끈다.
static bool split_on = true;
static bool select_start_view = false;
static uint32_t last_ms = 0;
static uint32_t frame_no = 0;
static TaskHandle_t task = nullptr;
static volatile Link via = LINK_NONE;  // 이번 장을 보낼 길 (USB / Wi-Fi)
static WiFiClient tcp;  // Wi-Fi: PC 의 4011 번 (동영상과 같은 받는 곳)
// 5초마다 기록에 남기는 걸린 시간
static uint32_t stat_frames = 0, stat_pack_ms = 0, stat_send_ms = 0, stat_bytes = 0, stat_ms = 0;

// 같은 색이 이어지는 것만 줄인다 (PackBits 식, 손실 없음). 이 화면은 단색 면이 많아 JPEG 보다 10배 넘게 빠르고 크기는 비슷하다.
// 머리 바이트 h < 128 이면 그 뒤 h+1 개 점을 그대로, 아니면 그 뒤 한 점을 h-126 번 (2~129) 반복. 점은 RGB565 작은 끝.
static size_t pack(const uint16_t *px, int n, uint8_t *out) {
  size_t o = 0;
  int i = 0;
  while (i < n) {
    int run = 1;
    while (i + run < n && run < 129 && px[i + run] == px[i]) run++;
    if (run >= 2) {
      out[o++] = 126 + run;
      memcpy(out + o, px + i, 2);
      o += 2;
      i += run;
      continue;
    }
    int start = i, lit = 0;
    while (i < n && lit < 128 && !(i + 1 < n && px[i + 1] == px[i])) i++, lit++;
    out[o++] = lit - 1;
    memcpy(out + o, px + start, lit * 2);
    o += lit * 2;
  }
  return o;
}

// Wi-Fi 는 작은 조각을 따로 보내면 느리므로 한 장의 조각들을 이어 붙여 한 번에 보낸다
static void send_wifi(const uint8_t *data, size_t len, Head head) {
  if (!tcp.connected()) {
    tcp.stop();
    if (!tcp.connect(mic_target(), VIDEO_TCP_PORT, 2000)) {
      Serial.println("[mirror] cannot reach the PC over TCP");
      return;
    }
    if (!net_tcp_hello(tcp)) {
      Serial.println("[mirror] PC refused Wi-Fi auth");
      tcp.stop();
      return;
    }
    tcp.setNoDelay(true);
  }
  size_t chunks = (len + CHUNK - 1) / CHUNK;
  uint8_t *buf = (uint8_t *)ps_malloc(len + chunks * (5 + sizeof(Head)));
  if (!buf) return;
  size_t pos = 0;
  for (size_t off = 0; off < len; off += CHUNK) {
    size_t n = min((size_t)CHUNK, len - off);
    size_t body = sizeof(Head) + n;
    head.offset = off;
    uint8_t frame_head[5] = {0xA5, 0x5A, USB_FRAME_MIRROR, (uint8_t)(body & 0xFF), (uint8_t)(body >> 8)};
    memcpy(buf + pos, frame_head, 5);
    memcpy(buf + pos + 5, &head, sizeof(Head));
    memcpy(buf + pos + 5 + sizeof(Head), data + off, n);
    pos += 5 + body;
  }
  tcp.write(buf, pos);
  free(buf);
}

static void send_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t t0 = millis();
    size_t len = pack(grab, grab_w * grab_h, packed);
    uint32_t t1 = millis();
    Head head = {frame_no++, 0, (uint32_t)len, (uint16_t)grab_w, (uint16_t)grab_h};
    if (via == LINK_WIFI) {
      send_wifi(packed, len, head);
    } else {
      for (size_t off = 0; off < len; off += CHUNK) {
        head.offset = off;
        usb_send_frame(USB_FRAME_MIRROR, (const uint8_t *)&head, sizeof(head), packed + off, min((size_t)CHUNK, len - off));
      }
    }
    stat_frames++;
    stat_pack_ms += t1 - t0;
    stat_send_ms += millis() - t1;
    stat_bytes += len;
    if (millis() - stat_ms > 5000) {
      if (stat_frames > 1) {
        Serial.printf("[mirror] %lu frames in 5s, pack %lu ms, send %lu ms, %lu KB each (%s)\n", (unsigned long)stat_frames,
                      (unsigned long)(stat_pack_ms / stat_frames), (unsigned long)(stat_send_ms / stat_frames),
                      (unsigned long)(stat_bytes / stat_frames / 1024), via == LINK_WIFI ? "Wi-Fi" : "USB");
      }
      stat_frames = stat_pack_ms = stat_send_ms = stat_bytes = 0;
      stat_ms = millis();
    }
    busy = false;
  }
}

// ---- 미러링 중 기기 화면 (display_device_screen) ----

static lv_obj_t *dev_clock, *dev_body, *dev_btn;
static uint32_t dev_version = 0;
static int dev_w = 0, dev_h = 0;

static void device_build() {
  lv_obj_t *scr = display_device_screen();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "PC 미러링 중");
  lv_obj_set_style_text_font(title, &font_ko_14, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x8A939E), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 8);
  dev_clock = lv_label_create(scr);
  lv_label_set_text(dev_clock, "");
  lv_obj_set_style_text_font(dev_clock, &font_ko_14, 0);
  lv_obj_set_style_text_color(dev_clock, lv_color_hex(0x8A939E), 0);
  dev_btn = ui_split_button(scr);
  lv_obj_align(dev_btn, LV_ALIGN_TOP_RIGHT, -6, 4);
  // 사용량 페이지들 (위아래로 넘긴다)
  dev_body = lv_tileview_create(scr);
  lv_obj_set_style_bg_color(dev_body, lv_color_hex(0x101418), 0);
  lv_obj_set_pos(dev_body, 0, DEVICE_STATUS_H);
}

// 1초마다: 시각, 그리고 사용량이나 화면 방향이 바뀌었으면 페이지를 새로 만든다
static void device_refresh() {
  static uint32_t refreshed_ms = 0;
  if (dev_body && millis() - refreshed_ms < 1000) return;
  refreshed_ms = millis();
  if (!dev_body) device_build();
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm t;
    localtime_r(&now, &t);
    lv_label_set_text_fmt(dev_clock, "%02d:%02d", t.tm_hour, t.tm_min);
  }
  int w = display_width(), h = display_height();
  if (dev_version == ui_usage_version() && dev_w == w && dev_h == h && lv_obj_get_child_cnt(dev_body)) return;
  // 넘기는 중에 다시 만들면 손 밑의 페이지가 사라지므로 다음 번으로 미룬다
  if (display_device_pressed() || lv_anim_get(dev_body, NULL)) return;
  dev_version = ui_usage_version();
  dev_w = w;
  dev_h = h;
  lv_obj_set_size(dev_body, w, h - DEVICE_STATUS_H);
  // 화면 방향이 바뀌면 버튼 자리도 바뀌므로 시계를 다시 버튼 왼쪽에 둔다
  lv_obj_update_layout(dev_btn);
  lv_obj_align_to(dev_clock, dev_btn, LV_ALIGN_OUT_LEFT_MID, -8, 0);
  ui_usage_pages(dev_body);
}

void mirror_init() {
  grab = (uint16_t *)ps_malloc(MAX_PIXELS * 2);
  // 가장 나쁜 경우 (모든 점이 달라 그대로 쓸 때) 128 점마다 머리 1 바이트가 붙는다
  packed = (uint8_t *)ps_malloc(MAX_PIXELS * 2 + MAX_PIXELS / 128 + 16);
  if (grab && packed) xTaskCreatePinnedToCore(send_task, "mirror", 6144, nullptr, 1, &task, 0);
}

void mirror_keep(bool on) {
  if (!task) return;
  bool changed = on != (until_ms != 0);
  if (changed) Serial.printf("[mirror] %s\n", on ? "on" : "off");
  // 새로 연결되면 기본은 나눠 쓰기 (기기 LCD 에는 사용량)
  if (changed && on) {
    split_on = true;
    select_start_view = true;
  }
  until_ms = on ? millis() + KEEP_MS : 0;
  display_mirror(on);
  if (!on) {
    select_start_view = false;
    display_split(false);
    pcpanel_device_watch(false);
  }
  if (changed) ui_mirror_changed();
}

bool mirror_active() {
  return until_ms != 0;
}

bool mirror_split() {
  return split_on;
}

void mirror_set_split(bool on) {
  if (on == split_on) return;
  split_on = on;
  Serial.printf("[mirror] split %s\n", on ? "on" : "off");
  ui_mirror_changed();
}

void mirror_loop() {
  if (!until_ms) return;
  Link now = link_active();
  if ((int32_t)(millis() - until_ms) > 0 || (now != LINK_USB && now != LINK_WIFI)) {
    mirror_keep(false);
    return;
  }
  // 처음 상태가 미러링 시작보다 늦게 와도 첫 화면을 고른다. 이후 상태 갱신은 화면을 바꾸지 않는다.
  if (select_start_view && orcapanel_received()) {
    select_start_view = false;
    if (orcapanel_running()) ui_show("orca");
  }
  if (split_on) device_refresh();
  display_split(split_on);
  // 기기 화면에서 PC 상태를 보는 동안만 PC 에 계속 보내 달라고 한다
  pcpanel_device_watch(split_on && dev_body && ui_usage_pages_on_pc(dev_body));
  if (busy || millis() - last_ms < MIN_GAP_MS) return;
  if (!display_mirror_take(grab, &grab_w, &grab_h)) return;
  last_ms = millis();
  via = now;
  busy = true;
  xTaskNotifyGive(task);
}
