#include "gallery.h"

#include <SD.h>
#include <img_converters.h>
#include <lvgl.h>

#include <algorithm>
#include <vector>

#include "display.h"
#include "health.h"
#include "storage.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define OUT_MAX_PIXELS (320 * 240)
// 풀 때 이보다 크지 않게 줄인다 (1600x1200 사진은 1/4 = 400x300 으로 풀어 화면에 맞게 다시 줄인다)
#define DECODE_MAX_PIXELS (400 * 300)
// 밀기 바로 뒤에 오는 누르기는 재생·멈춤으로 보지 않는다
#define GESTURE_CLICK_MS 400
// 지우기는 이 시간 안에 두 번 눌러야 한다
#define DELETE_CONFIRM_MS 3000

struct Item {
  String path;
  String key;  // 이름에서 "IMG_" / "VID_" 를 뺀 것 (찍은 시각 순으로 정렬된다)
  bool video;
};

static std::vector<Item> items;
static int current = 0;
static bool is_open = false;
static void (*close_cb)() = nullptr;
static uint32_t gestured_ms = 0;
static uint32_t delete_armed_ms = 0;  // 지우기를 한 번 누른 시각 (0 이면 안 누름)

static lv_obj_t *overlay, *img, *close_btn, *delete_btn, *prev_btn, *next_btn, *info_lbl, *msg_lbl, *play_icon, *time_lbl;
static lv_img_dsc_t dsc;
static uint16_t *out_buf = nullptr;  // 화면에 보이는 그림 (PSRAM)
static uint8_t *dec_buf = nullptr;   // 풀어 놓은 JPEG (PSRAM)
static uint8_t *jpg_buf = nullptr;   // 카드에서 읽은 JPEG (PSRAM, 필요할 때 늘린다)
static size_t jpg_cap = 0;

// 동영상: sd_video.cpp 가 쓴 MOV 의 장 위치·크기·시작 시각(ms)
static File video;
static uint32_t *v_off = nullptr, *v_size = nullptr, *v_start = nullptr;
static uint32_t v_count = 0, v_duration = 0;
static uint8_t v_turns = 0;
static int v_shown = -1;
static bool playing = false;
static uint32_t play_from = 0;  // 재생 중: 0:00 이었을 millis
static uint32_t paused_at = 0;  // 멈춘 자리 (ms)

static uint16_t be16(const uint8_t *p) {
  return (p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// JPEG 머리(SOF)에서 가로·세로를 읽는다
static bool jpeg_size(const uint8_t *d, size_t n, int *w, int *h) {
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 9 < n && d[i] == 0xFF) {
    uint8_t marker = d[i + 1];
    if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
      *h = be16(d + i + 5);
      *w = be16(d + i + 7);
      return *w > 0 && *h > 0;
    }
    i += 2 + be16(d + i + 2);
  }
  return false;
}

// 사진의 회전 정보(EXIF Orientation, camera.cpp 의 add_orientation)를 시계 방향 90도 횟수로
static uint8_t exif_turns(const uint8_t *d, size_t n) {
  if (n < 40 || d[2] != 0xFF || d[3] != 0xE1 || memcmp(d + 6, "Exif\0\0", 6) != 0) return 0;
  const uint8_t *tiff = d + 12;
  size_t room = min(n - 12, (size_t)(be16(d + 4) - 8));
  bool little = tiff[0] == 'I';
  auto rd16 = [&](size_t at) -> uint16_t { return little ? tiff[at] | (tiff[at + 1] << 8) : be16(tiff + at); };
  auto rd32 = [&](size_t at) -> uint32_t { return little ? rd16(at) | ((uint32_t)rd16(at + 2) << 16) : be32(tiff + at); };
  uint32_t ifd = rd32(4);
  if (ifd + 2 > room) return 0;
  uint16_t entries = rd16(ifd);
  for (uint16_t i = 0; i < entries && ifd + 2 + 12 * (i + 1) <= room; i++) {
    size_t e = ifd + 2 + 12 * i;
    if (rd16(e) != 0x0112) continue;
    switch (rd16(e + 8)) {
      case 6: return 1;
      case 3: return 2;
      case 8: return 3;
      default: return 0;
    }
  }
  return 0;
}

// JPEG 를 k 번(시계 방향 90도) 돌려 fit_w x fit_h 안에 들어가게 out_buf 에 그린다
static bool decode_fit(const uint8_t *jpg, size_t len, uint8_t k, int fit_w, int fit_h, int *ow, int *oh) {
  static const esp_jpeg_image_scale_t SCALES[] = {JPG_SCALE_NONE, JPG_SCALE_2X, JPG_SCALE_4X, JPG_SCALE_8X};
  int sw, sh;
  if (!jpeg_size(jpg, len, &sw, &sh)) return false;
  int rw = (k & 1) ? sh : sw, rh = (k & 1) ? sw : sh;
  float f = min(1.0f, min((float)fit_w / rw, (float)fit_h / rh));
  // 화면에 그릴 크기보다 작아지지 않는 한 크게 줄여서 푼다 (빠르다)
  int s = 0;
  while (s < 3 && ((2 << s) * f <= 1.0f || (sw >> s) * (sh >> s) > DECODE_MAX_PIXELS)) s++;
  int dw = sw >> s, dh = sh >> s;
  if (dw * dh > DECODE_MAX_PIXELS || !jpg2rgb565(jpg, len, dec_buf, SCALES[s])) return false;

  int rdw = (k & 1) ? dh : dw, rdh = (k & 1) ? dw : dh;
  float g = min(1.0f, min((float)fit_w / rdw, (float)fit_h / rdh));
  int w = max(1, (int)(rdw * g)), h = max(1, (int)(rdh * g));
  for (int oy = 0; oy < h; oy++) {
    int ry = oy * rdh / h;
    for (int ox = 0; ox < w; ox++) {
      int rx = ox * rdw / w;
      int x, y;
      switch (k & 3) {
        case 1: x = ry; y = dh - 1 - rx; break;
        case 2: x = dw - 1 - rx; y = dh - 1 - ry; break;
        case 3: x = dw - 1 - ry; y = rx; break;
        default: x = rx; y = ry; break;
      }
      // 풀어 놓은 그림은 이미 화면과 같은 바이트 순서다
      const uint8_t *p = dec_buf + 2 * (y * dw + x);
      out_buf[oy * w + ox] = p[0] | (p[1] << 8);
    }
  }
  *ow = w;
  *oh = h;
  return true;
}

static void show_message(const char *text) {
  if (text) {
    lv_label_set_text(msg_lbl, text);
    lv_obj_clear_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
  }
}

static bool show_jpeg(const uint8_t *jpg, size_t len, uint8_t turns) {
  int w, h;
  if (!decode_fit(jpg, len, turns, display_width(), display_height(), &w, &h)) return false;
  dsc.header.w = w;
  dsc.header.h = h;
  dsc.data_size = w * h * 2;
  lv_img_set_src(img, &dsc);
  lv_obj_center(img);
  lv_obj_invalidate(img);
  lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
  return true;
}

static bool read_into_buf(File &f, uint32_t at, uint32_t len) {
  if (len > jpg_cap) {
    uint8_t *p = (uint8_t *)ps_realloc(jpg_buf, len);
    if (!p) return false;
    jpg_buf = p;
    jpg_cap = len;
  }
  return f.seek(at) && f.read(jpg_buf, len) == len;
}

// ---- 동영상 ----

// 상자 안의 바로 아래 상자 중 type 을 찾아 내용(머리 8바이트 뒤)을 돌려준다
static const uint8_t *child(const uint8_t *p, size_t n, const char *type, size_t *len) {
  size_t i = 0;
  while (i + 8 <= n) {
    uint32_t box = be32(p + i);
    if (box < 8 || i + box > n) return nullptr;
    if (memcmp(p + i + 4, type, 4) == 0) {
      *len = box - 8;
      return p + i + 8;
    }
    i += box;
  }
  return nullptr;
}

static void close_video() {
  if (video) video.close();
  free(v_off), free(v_size), free(v_start);
  v_off = v_size = v_start = nullptr;
  v_count = v_duration = 0;
  v_shown = -1;
  playing = false;
  paused_at = 0;
}

static bool parse_moov(const uint8_t *moov, size_t n) {
  size_t len;
  const uint8_t *trak = child(moov, n, "trak", &len);
  if (!trak) return false;
  size_t trak_len = len;
  const uint8_t *tkhd = child(trak, trak_len, "tkhd", &len);
  if (tkhd && len >= 48) {
    // 변환 행렬의 a, b 로 돌린 횟수를 안다 (sd_video.cpp 의 matrix)
    int32_t a = (int32_t)be32(tkhd + 40), b = (int32_t)be32(tkhd + 44);
    v_turns = a > 0 ? 0 : b > 0 ? 1 : a < 0 ? 2 : 3;
  }
  const uint8_t *p = trak;
  size_t pn = trak_len;
  for (const char *type : {"mdia", "minf", "stbl"}) {
    p = child(p, pn, type, &pn);
    if (!p) return false;
  }
  size_t stsz_len, stco_len, stts_len;
  const uint8_t *stsz = child(p, pn, "stsz", &stsz_len);
  const uint8_t *stco = child(p, pn, "stco", &stco_len);
  const uint8_t *stts = child(p, pn, "stts", &stts_len);
  if (!stsz || !stco || !stts || stsz_len < 12 || stco_len < 8 || stts_len < 8) return false;
  uint32_t n_frames = be32(stsz + 8);
  if (!n_frames || be32(stco + 4) != n_frames || stsz_len < 12 + 4 * n_frames || stco_len < 8 + 4 * n_frames) return false;
  v_off = (uint32_t *)ps_malloc(n_frames * 4);
  v_size = (uint32_t *)ps_malloc(n_frames * 4);
  v_start = (uint32_t *)ps_malloc(n_frames * 4);
  if (!v_off || !v_size || !v_start) return false;
  for (uint32_t i = 0; i < n_frames; i++) {
    v_size[i] = be32(stsz + 12 + 4 * i);
    v_off[i] = be32(stco + 8 + 4 * i);
  }
  // 장마다 머무는 시간 (같은 시간이 이어지는 것끼리 묶여 있다)
  uint32_t runs = be32(stts + 4), t = 0, i = 0;
  for (uint32_t r = 0; r < runs && 8 + 8 * (r + 1) <= stts_len; r++) {
    uint32_t cnt = be32(stts + 8 + 8 * r), delta = be32(stts + 12 + 8 * r);
    for (uint32_t c = 0; c < cnt && i < n_frames; c++, i++) {
      v_start[i] = t;
      t += delta;
    }
  }
  for (; i < n_frames; i++) v_start[i] = t, t += 100;
  v_count = n_frames;
  v_duration = t;
  return true;
}

static bool open_video(const char *path) {
  video = SD.open(path);
  if (!video) return false;
  uint32_t size = video.size(), pos = 0;
  uint8_t head[8];
  while (pos + 8 <= size) {
    if (!video.seek(pos) || video.read(head, 8) != 8) return false;
    uint32_t box = be32(head);
    if (box < 8) return false;  // 녹화가 끝나지 않은 파일 (전원이 꺼졌을 때)
    if (memcmp(head + 4, "moov", 4) == 0) {
      uint8_t *moov = (uint8_t *)ps_malloc(box - 8);
      bool ok = moov && video.read(moov, box - 8) == box - 8 && parse_moov(moov, box - 8);
      free(moov);
      return ok;
    }
    pos += box;
  }
  return false;
}

static void format_time(char *out, size_t size, uint32_t ms) {
  snprintf(out, size, "%lu:%02lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60));
}

static void show_video_time(uint32_t pos) {
  char a[12], b[12], t[28];
  format_time(a, sizeof(a), pos);
  format_time(b, sizeof(b), v_duration);
  snprintf(t, sizeof(t), "%s / %s", a, b);
  if (strcmp(lv_label_get_text(time_lbl), t) != 0) lv_label_set_text(time_lbl, t);
}

static bool show_video_frame(int i) {
  v_shown = i;
  return read_into_buf(video, v_off[i], v_size[i]) && show_jpeg(jpg_buf, v_size[i], v_turns);
}

static void set_playing(bool on) {
  if (!v_count) return;
  if (on) {
    // 끝까지 봤으면 처음부터
    if (paused_at >= v_duration || v_shown == (int)v_count - 1) {
      paused_at = 0;
      show_video_frame(0);
    }
    play_from = millis() - paused_at;
  } else if (playing) {
    paused_at = millis() - play_from;
  }
  playing = on;
  if (on) {
    lv_obj_add_flag(play_icon, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(play_icon, LV_OBJ_FLAG_HIDDEN);
  }
}

// ---- 목록과 넘기기 ----

static void scan(const char *dir, const char *ext, bool is_video) {
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String name = f.name();
    // Mac 이 카드에 남기는 "._IMG..." 같은 숨은 파일은 뺀다
    bool ok = !f.isDirectory() && f.size() > 0 && name.length() > 4 && name[0] != '.' && name.endsWith(ext);
    f.close();
    if (ok) items.push_back({String(dir) + "/" + name, name.substring(4), is_video});
    health_watch_feed();  // 파일이 많으면 오래 걸린다
  }
  d.close();
}

static void disarm_delete() {
  if (!delete_armed_ms) return;
  delete_armed_ms = 0;
  lv_obj_set_style_bg_color(delete_btn, lv_color_black(), 0);
  lv_obj_add_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void load_item(int i) {
  close_video();
  disarm_delete();
  lv_obj_add_flag(play_icon, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(time_lbl, LV_OBJ_FLAG_HIDDEN);
  if (items.empty()) {
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(delete_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prev_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(next_btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(info_lbl, "");
    show_message(storage_ready() ? "SD 카드에 사진·동영상이 없습니다" : "SD 카드가 없습니다");
    return;
  }
  current = constrain(i, 0, (int)items.size() - 1);
  lv_obj_clear_flag(delete_btn, LV_OBJ_FLAG_HIDDEN);
  const Item &item = items[current];
  // 위: "3/12 · 10월 19일 14:05" (이름에 시각이 없으면 파일 이름)
  const String &k = item.key;
  char when[40];
  if (k.length() >= 15 && k[8] == '_') {
    snprintf(when, sizeof(when), "%d월 %d일 %s:%s", k.substring(4, 6).toInt(), k.substring(6, 8).toInt(),
             k.substring(9, 11).c_str(), k.substring(11, 13).c_str());
  } else {
    strlcpy(when, item.path.substring(item.path.lastIndexOf('/') + 1).c_str(), sizeof(when));
  }
  lv_label_set_text_fmt(info_lbl, "%s%d/%d · %s", item.video ? "동영상 " : "", current + 1, (int)items.size(), when);
  if (current > 0) {
    lv_obj_clear_flag(prev_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(prev_btn, LV_OBJ_FLAG_HIDDEN);
  }
  if (current + 1 < (int)items.size()) {
    lv_obj_clear_flag(next_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(next_btn, LV_OBJ_FLAG_HIDDEN);
  }

  show_message("불러오는 중...");
  lv_refr_now(NULL);
  uint32_t t0 = millis();
  bool ok;
  if (item.video) {
    ok = open_video(item.path.c_str()) && show_video_frame(0);
    if (ok) {
      show_video_time(0);
      lv_obj_clear_flag(time_lbl, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(play_icon, LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    File f = SD.open(item.path);
    ok = f && read_into_buf(f, 0, f.size()) && show_jpeg(jpg_buf, f.size(), exif_turns(jpg_buf, f.size()));
    if (f) f.close();
  }
  Serial.printf("[gallery] %s %s (%lu ms)\n", item.path.c_str(), ok ? "shown" : "failed", (unsigned long)(millis() - t0));
  if (ok) {
    show_message(nullptr);
  } else {
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    show_message(item.video ? "열 수 없는 동영상입니다" : "열 수 없는 사진입니다");
  }
}

// 처음 누르면 빨갛게 바뀌고 "한 번 더" 를 알린다. 3초 안에 다시 누르면 카드에서 지우고 다음 것을 보여 준다.
static void delete_clicked(lv_event_t *e) {
  if (items.empty()) return;
  if (!delete_armed_ms) {
    delete_armed_ms = millis();
    if (playing) set_playing(false);
    lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0xC0201A), 0);
    show_message(items[current].video ? "한 번 더 누르면 이 동영상을 지웁니다" : "한 번 더 누르면 이 사진을 지웁니다");
    return;
  }
  String path = items[current].path;
  close_video();  // 열린 파일은 지울 수 없다
  bool ok = SD.remove(path);
  Serial.printf("[gallery] delete %s: %s\n", path.c_str(), ok ? "ok" : "failed");
  if (!ok) {
    disarm_delete();
    show_message("지우지 못했습니다");
    return;
  }
  items.erase(items.begin() + current);
  load_item(min(current, (int)items.size() - 1));
}

static void close_gallery() {
  if (!is_open) return;
  is_open = false;
  close_video();
  items.clear();
  items.shrink_to_fit();
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  free(out_buf), free(dec_buf), free(jpg_buf);
  out_buf = nullptr, dec_buf = nullptr, jpg_buf = nullptr;
  jpg_cap = 0;
  if (close_cb) close_cb();
}

static void go(int step) {
  int next = current + step;
  if (next < 0 || next >= (int)items.size()) return;
  load_item(next);
}

static void overlay_gesture(lv_event_t *e) {
  gestured_ms = millis();
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  // 사진 앱처럼 왼쪽으로 밀면 다음(더 오래된) 것, 오른쪽으로 밀면 앞(더 최근) 것, 아래로 밀면 닫기
  if (dir == LV_DIR_LEFT) {
    go(1);
  } else if (dir == LV_DIR_RIGHT) {
    go(-1);
  } else if (dir == LV_DIR_BOTTOM) {
    close_gallery();
  }
}

static void overlay_clicked(lv_event_t *e) {
  if (millis() - gestured_ms < GESTURE_CLICK_MS) return;
  if (v_count) set_playing(!playing);
}

static lv_obj_t *round_button(const char *symbol, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(overlay);
  lv_obj_set_size(btn, 36, 36);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_ext_click_area(btn, 8);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, symbol);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_center(lbl);
  return btn;
}

static lv_obj_t *caption(const lv_font_t *font) {
  lv_obj_t *lbl = lv_label_create(overlay);
  lv_label_set_text(lbl, "");
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(lbl, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(lbl, LV_OPA_60, 0);
  lv_obj_set_style_radius(lbl, 4, 0);
  lv_obj_set_style_pad_hor(lbl, 6, 0);
  return lbl;
}

void gallery_init() {
  dsc.header.cf = LV_IMG_CF_TRUE_COLOR;

  // 카메라 화면보다 위 (같은 층에서 나중에 만든 것이 위)
  overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(overlay);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(overlay, overlay_gesture, LV_EVENT_GESTURE, NULL);
  lv_obj_add_event_cb(overlay, overlay_clicked, LV_EVENT_CLICKED, NULL);

  img = lv_img_create(overlay);

  // 동영상을 멈춰 두었을 때 가운데에 재생 표시
  play_icon = lv_obj_create(overlay);
  lv_obj_remove_style_all(play_icon);
  lv_obj_set_size(play_icon, 64, 64);
  lv_obj_set_style_radius(play_icon, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(play_icon, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(play_icon, LV_OPA_60, 0);
  lv_obj_clear_flag(play_icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *sym = lv_label_create(play_icon);
  lv_label_set_text(sym, LV_SYMBOL_PLAY);
  lv_obj_set_style_text_font(sym, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(sym, lv_color_white(), 0);
  lv_obj_align(sym, LV_ALIGN_CENTER, 3, 0);

  close_btn = round_button(LV_SYMBOL_CLOSE, [](lv_event_t *e) { close_gallery(); });
  delete_btn = round_button(LV_SYMBOL_TRASH, delete_clicked);
  prev_btn = round_button(LV_SYMBOL_LEFT, [](lv_event_t *e) { go(-1); });
  next_btn = round_button(LV_SYMBOL_RIGHT, [](lv_event_t *e) { go(1); });
  info_lbl = caption(&font_ko_14);
  time_lbl = caption(&font_ko_14);
  msg_lbl = caption(&font_ko_20);
  lv_obj_set_style_pad_all(msg_lbl, 8, 0);

  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

void gallery_layout() {
  int w = display_width(), h = display_height();
  lv_obj_set_size(overlay, w, h);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 6, 6);
  lv_obj_align(info_lbl, LV_ALIGN_TOP_MID, 22, 14);
  lv_obj_align(prev_btn, LV_ALIGN_LEFT_MID, 6, 0);
  lv_obj_align(next_btn, LV_ALIGN_RIGHT_MID, -6, 0);
  lv_obj_align(delete_btn, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
  lv_obj_align(time_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_center(play_icon);
  lv_obj_center(msg_lbl);
  // 화면 방향이 바뀌면 지금 것을 새 크기로 다시 그린다
  if (is_open) load_item(current);
}

void gallery_show(void (*on_close)()) {
  if (is_open) return;
  close_cb = on_close;
  out_buf = (uint16_t *)ps_malloc(OUT_MAX_PIXELS * 2);
  dec_buf = (uint8_t *)ps_malloc(DECODE_MAX_PIXELS * 2);
  if (!out_buf || !dec_buf) {
    free(out_buf), free(dec_buf);
    out_buf = nullptr, dec_buf = nullptr;
    return;
  }
  dsc.data = (const uint8_t *)out_buf;
  display_poke();
  gallery_layout();
  is_open = true;
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(play_icon, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(time_lbl, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(prev_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(next_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(delete_btn, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(info_lbl, "");
  show_message("불러오는 중...");
  lv_refr_now(NULL);

  items.clear();
  if (storage_ready()) {
    scan("/photos", ".jpg", false);
    scan("/videos", ".mov", true);
  }
  // 최근 것부터
  std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.key > b.key; });
  Serial.printf("[gallery] %u items\n", (unsigned)items.size());
  load_item(0);
}

bool gallery_open() {
  return is_open;
}

void gallery_loop() {
  if (is_open && delete_armed_ms && millis() - delete_armed_ms > DELETE_CONFIRM_MS) disarm_delete();
  if (!is_open || !playing) return;
  uint32_t pos = millis() - play_from;
  display_poke();  // 보는 동안 화면 보호기·절전이 끼어들지 않게
  if (pos >= v_duration) {
    set_playing(false);
    show_video_time(v_duration);
    return;
  }
  // 늦어졌으면 그 사이 장은 건너뛴다
  int i = max(v_shown, 0);
  while (i + 1 < (int)v_count && v_start[i + 1] <= pos) i++;
  if (i != v_shown) show_video_frame(i);
  show_video_time(pos);
}
