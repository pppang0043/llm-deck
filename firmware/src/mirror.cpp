#include "mirror.h"

#include <WiFi.h>
#include <lvgl.h>
#include <time.h>

#include "askpanel.h"
#include "display.h"
#include "link.h"
#include "mic.h"
#include "net.h"
#include "orcapanel.h"
#include "pcmic.h"
#include "pcpanel.h"
#include "ui.h"
#include "usb_link.h"
#include "video.h"
#include "quickpanel.h"

// 창이 이만큼 아무 말이 없으면 (닫혔거나 도우미가 꺼짐) 끈다
#define KEEP_MS 6000
// 장 사이의 최소 간격 (초당 최대 약 25장). 화면 복사가 화면 반응을 늦추지 않게 둔다.
#define MIN_GAP_MS 40
// Wi-Fi 신호가 약하면 (net_wifi_bars) 장 사이를 벌리고, 아주 약하면 색을 줄여 한 장을 작게 만든다.
// 신호 -90dBm 대에서 18KB 장은 거의 다 깨졌고, 작은 묶음만 지나갔다 (2026-10-26).
static const uint16_t WIFI_GAP_MS[5] = {2500, 1000, 250, 40, 40};  // 0~4칸
#define WEAK_COLOR_MASK 0xE79C  // RGB565 에서 R·B 위 3비트, G 위 4비트만 (같은 색이 길게 이어져 덜 커진다)
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
static uint32_t split_sent_ms = 0;  // 나눠 쓰기 상태를 PC 에 알린 시각 (0 = 바로 알린다)
static bool show_orca = false;  // 합치기에서 분리로 바꾸면 PC 창은 ORCA 로 (버튼 이벤트 안에서 화면을 바꾸지 않게 loop 에서)
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
  // 약한 신호에서 10초 넘게 못 보내면 write 가 쓰다 말고 돌아온다. 그 연결은 재전송이 밀려 있을 수 있으니 버리고 다음 장에서 새로 잇는다
  size_t sent = tcp.write(buf, pos);
  free(buf);
  if (sent != pos) {
    Serial.printf("[mirror] Wi-Fi write stopped at %u of %u bytes, reconnecting\n", (unsigned)sent, (unsigned)pos);
    tcp.stop();
  }
}

static void send_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t t0 = millis();
    if (via == LINK_WIFI && net_wifi_bars() <= 1) {
      for (int i = 0; i < grab_w * grab_h; i++) grab[i] &= WEAK_COLOR_MASK;
    }
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
static lv_obj_t *dev_title = nullptr;
static lv_obj_t *dev_ring = nullptr;  // 두 번 탭을 알아들었다는 표시
static lv_point_t ring_at;

static void device_build() {
  lv_obj_t *scr = display_device_screen();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  dev_title = lv_label_create(scr);
  lv_label_set_text(dev_title, "");
  lv_obj_set_style_text_font(dev_title, &font_ko_14, 0);
  lv_obj_set_style_text_color(dev_title, lv_color_hex(0x8A939E), 0);
  lv_obj_align(dev_title, LV_ALIGN_TOP_LEFT, 8, 8);
  lv_obj_set_height(dev_title, 16);
  lv_label_set_long_mode(dev_title, LV_LABEL_LONG_DOT);
  pcmic_build_device(scr);
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
  dev_ring = lv_obj_create(scr);
  lv_obj_set_style_radius(dev_ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dev_ring, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(dev_ring, LV_OPA_30, 0);
  lv_obj_set_style_border_color(dev_ring, lv_color_white(), 0);
  lv_obj_set_style_border_width(dev_ring, 3, 0);
  lv_obj_clear_flag(dev_ring, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_add_flag(dev_ring, LV_OBJ_FLAG_HIDDEN);
}

static void ring_step(void *, int32_t v) {
  int size = 16 + 56 * v / 255;
  lv_obj_set_size(dev_ring, size, size);
  lv_obj_set_pos(dev_ring, ring_at.x - size / 2, ring_at.y - size / 2);
  lv_obj_set_style_opa(dev_ring, 255 - v, 0);
}

// 누른 자리에서 원이 퍼지며 사라진다
static void show_ring(const lv_point_t &at) {
  ring_at = at;
  lv_obj_clear_flag(dev_ring, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(dev_ring);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, dev_ring);
  lv_anim_set_exec_cb(&a, ring_step);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_time(&a, 400);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, [](lv_anim_t *) { lv_obj_add_flag(dev_ring, LV_OBJ_FLAG_HIDDEN); });
  lv_anim_start(&a);
}

// 기기 화면을 두 번 탭: 받아쓰기 시작, 말하는 중이면 입력. 버튼 위에서 두 번 누른 것은 그 버튼만 눌린다.
static void device_dictate() {
  lv_point_t at;
  if (!display_device_double_tap(&at) || !dev_body) return;
  lv_obj_t *hit = lv_indev_search_obj(display_device_screen(), &at);
  if (hit && lv_obj_check_type(hit, &lv_btn_class)) return;
  show_ring(at);
  if (askpanel_talk_state() != ASK_TALKING) {
    askpanel_press(true);
  } else if (askpanel_dictating()) {
    askpanel_release();
  }
}

// 받아쓰기 버튼이 없는 화면이라 제목 자리에 진행을 보인다
static void device_title() {
  if (!dev_title) return;
  int st = askpanel_talk_state(), step = askpanel_dictate_step();
  const char *text = ui_status_text();  // 평소에는 미러링 화면과 같은 연결 상태 (USB, Wi-Fi)
  if (st == ASK_TALKING) {
    text = step == DICTATE_PREPARING ? "마이크 준비 중..." : LV_SYMBOL_AUDIO " 말하세요";
  } else if (step == DICTATE_WRITING) {
    text = "받아 적는 중...";
  } else if (st == ASK_NO_PC) {
    text = "PC 연결 없음";
  } else if (st == ASK_NO_MIC) {
    text = "PC 마이크 없음";
  }
  // 같은 값을 넣어도 LVGL 은 다시 그린다. loop 마다 도는 곳이라 바뀔 때만 넣는다 (아니면 기기 LCD 를 쉬지 않고 그린다)
  if (strcmp(lv_label_get_text(dev_title), text)) lv_label_set_text(dev_title, text);
  static bool was_talking = false;
  bool talking = st == ASK_TALKING;
  if (talking != was_talking) {
    was_talking = talking;
    lv_obj_set_style_text_color(dev_title, talking ? lv_color_white() : lv_color_hex(0x8A939E), 0);
  }
  // 글자 너비만큼만 차지하고 바로 오른쪽에 마이크 표시. 시계까지 자리가 모자라면 글자를 줄인다
  int room = LV_MAX(1, lv_obj_get_x(dev_clock) - 16 - pcmic_device_width());
  int width = LV_MIN(room, lv_txt_get_width(text, strlen(text), &font_ko_14, 0, LV_TEXT_FLAG_NONE) + 1);
  static int placed_width = -1;
  if (width != placed_width) {
    placed_width = width;
    lv_obj_set_width(dev_title, width);
    pcmic_device_place(8 + width, 6);
  }
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
  lv_obj_update_layout(dev_btn);
  lv_obj_t *status = quickpanel_status_before(dev_btn);
  lv_obj_align_to(dev_clock, status, LV_ALIGN_OUT_LEFT_MID, -8, 0);
  lv_obj_update_layout(dev_clock);
  if (dev_version == ui_usage_version() && dev_w == w && dev_h == h && lv_obj_get_child_cnt(dev_body)) return;
  // 넘기는 중에 다시 만들면 손 밑의 페이지가 사라지므로 다음 번으로 미룬다
  if (display_device_pressed() || lv_anim_get(dev_body, NULL)) return;
  dev_version = ui_usage_version();
  dev_w = w;
  dev_h = h;
  lv_obj_set_size(dev_body, w, h - DEVICE_STATUS_H);
  ui_usage_pages(dev_body);
}

void mirror_init() {
  grab = (uint16_t *)ps_malloc(MAX_PIXELS * 2);
  // 가장 나쁜 경우 (모든 점이 달라 그대로 쓸 때) 128 점마다 머리 1 바이트가 붙는다
  packed = (uint8_t *)ps_malloc(MAX_PIXELS * 2 + MAX_PIXELS / 128 + 16);
  if (grab && packed) xTaskCreatePinnedToCoreWithCaps(send_task, "mirror", 6144, nullptr, 1, &task, 0, MALLOC_CAP_SPIRAM);
}

void mirror_keep(bool on) {
  if (!task) return;
  bool changed = on != (until_ms != 0);
  if (changed) Serial.printf("[mirror] %s\n", on ? "on" : "off");
  // 새로 연결되면 기본은 나눠 쓰기 (기기 LCD 에는 사용량)
  if (changed && on) {
    split_on = true;
    split_sent_ms = 0;
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
  split_sent_ms = 0;
  Serial.printf("[mirror] split %s\n", on ? "on" : "off");
  if (on && mirror_active()) show_orca = true;
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
  if (show_orca) {
    show_orca = false;
    if (split_on) ui_show("orca");
  }
  // 미러링 앱의 합치기·분리 단추가 쓰게 바뀌면 바로, 아니면 10초마다 (도우미를 다시 켜도 알게) 알린다
  if (!split_sent_ms || millis() - split_sent_ms >= 10000) {
    link_send_event(split_on ? "{\"mirror_split\":true}" : "{\"mirror_split\":false}");
    split_sent_ms = millis() | 1;
  }
  if (split_on) {
    device_refresh();
    device_dictate();
    device_title();
  }
  display_split(split_on);
  // 기기 화면에서 PC 상태를 보는 동안만 PC 에 계속 보내 달라고 한다
  pcpanel_device_watch(split_on && dev_body && ui_usage_pages_on_pc(dev_body));
  uint32_t gap = MIN_GAP_MS;
  if (now == LINK_WIFI) {
    int bars = net_wifi_bars();
    if (bars >= 0) gap = WIFI_GAP_MS[bars];
  }
  if (busy || millis() - last_ms < gap) return;
  if (!display_mirror_take(grab, &grab_w, &grab_h)) return;
  last_ms = millis();
  via = now;
  busy = true;
  xTaskNotifyGive(task);
}
