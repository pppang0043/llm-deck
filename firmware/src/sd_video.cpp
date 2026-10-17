#include "sd_video.h"

#include <SD.h>
#include <freertos/idf_additions.h>

#include "storage.h"

#define TIMESCALE 1000  // ms 단위
// 쓰기를 기다리는 장 수. 넘치면 그 장은 버린다.
#define QUEUE_FRAMES 10

struct Frame {
  uint8_t *data;  // nullptr 이면 "녹화 끝"
  size_t len;
  uint32_t ms;
};

static QueueHandle_t queue;
static File file;
static volatile bool active = false;   // 녹화 중 (장을 받는다)
static bool writing = false;           // 파일이 열려 있다 (끝 표시를 받을 때까지)
static uint8_t turns = 0;
static uint32_t mdat_at = 0;
static uint16_t width = 0, height = 0;
static volatile uint32_t dropped = 0;
static bool failed = false;
static uint32_t result_frames = 0, result_dropped = 0;
static bool result_failed = false;
static char path[64];

// 장마다의 위치·크기·시각 (PSRAM, 필요할 때 늘린다)
static uint32_t *offsets = nullptr, *sizes = nullptr, *times = nullptr;
static uint32_t count = 0, capacity = 0;

// ---- 큰 끝(big-endian) 상자 쓰기 ----

struct Buf {
  uint8_t *p = nullptr;
  size_t len = 0, cap = 0;
  void need(size_t n) {
    if (len + n <= cap) return;
    cap = max(cap * 2, len + n + 1024);
    p = (uint8_t *)ps_realloc(p, cap);
  }
  void u8(uint8_t v) {
    need(1);
    p[len++] = v;
  }
  void u16(uint16_t v) {
    u8(v >> 8);
    u8(v);
  }
  void u32(uint32_t v) {
    u16(v >> 16);
    u16(v);
  }
  void bytes(const void *d, size_t n) {
    need(n);
    memcpy(p + len, d, n);
    len += n;
  }
  void zeros(size_t n) {
    need(n);
    memset(p + len, 0, n);
    len += n;
  }
  size_t begin(const char *type) {
    size_t at = len;
    u32(0);
    bytes(type, 4);
    return at;
  }
  void end(size_t at) {
    uint32_t size = len - at;
    p[at] = size >> 24;
    p[at + 1] = size >> 16;
    p[at + 2] = size >> 8;
    p[at + 3] = size;
  }
};

// QuickTime 변환 행렬 (a b u / c d v / x y w). a~d, x, y 는 16.16, w 는 2.30 고정소수점.
static void matrix(Buf &b, uint8_t t, uint32_t w, uint32_t h) {
  const int32_t one = 0x10000;
  int32_t m[6];
  switch (t & 3) {
    case 1: m[0] = 0, m[1] = one, m[2] = -one, m[3] = 0, m[4] = h * one, m[5] = 0; break;
    case 2: m[0] = -one, m[1] = 0, m[2] = 0, m[3] = -one, m[4] = w * one, m[5] = h * one; break;
    case 3: m[0] = 0, m[1] = -one, m[2] = one, m[3] = 0, m[4] = 0, m[5] = w * one; break;
    default: m[0] = one, m[1] = 0, m[2] = 0, m[3] = one, m[4] = 0, m[5] = 0; break;
  }
  b.u32(m[0]), b.u32(m[1]), b.u32(0);
  b.u32(m[2]), b.u32(m[3]), b.u32(0);
  b.u32(m[4]), b.u32(m[5]), b.u32(0x40000000);
}

// JPEG 머리(SOF)에서 가로·세로를 읽는다
static void jpeg_size(const uint8_t *d, size_t n, uint16_t *w, uint16_t *h) {
  *w = 640, *h = 480;
  size_t i = 2;
  while (i + 9 < n && d[i] == 0xFF) {
    uint8_t marker = d[i + 1];
    uint16_t len = (d[i + 2] << 8) | d[i + 3];
    if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
      *h = (d[i + 5] << 8) | d[i + 6];
      *w = (d[i + 7] << 8) | d[i + 8];
      return;
    }
    i += 2 + len;
  }
}

static void write_moov() {
  uint32_t n = count;
  // 장마다 머무는 시간 = 다음 장과의 시각 차이. 마지막 장은 평균으로.
  uint32_t duration = 0;
  Buf stts;
  uint32_t runs = 0, run_count = 0, run_gap = 0;
  stts.zeros(8);  // 버전·플래그, 항목 수 (나중에 채움)
  for (uint32_t i = 0; i < n; i++) {
    uint32_t gap;
    if (i + 1 < n) {
      gap = max((uint32_t)1, times[i + 1] - times[i]);
    } else {
      gap = n > 1 ? max((uint32_t)1, (times[n - 1] - times[0]) / (n - 1)) : 100;
    }
    duration += gap;
    if (run_count && gap == run_gap) {
      run_count++;
    } else {
      if (run_count) stts.u32(run_count), stts.u32(run_gap), runs++;
      run_count = 1, run_gap = gap;
    }
  }
  if (run_count) stts.u32(run_count), stts.u32(run_gap), runs++;
  stts.p[4] = runs >> 24, stts.p[5] = runs >> 16, stts.p[6] = runs >> 8, stts.p[7] = runs;

  uint32_t disp_w = (turns & 1) ? height : width, disp_h = (turns & 1) ? width : height;
  Buf b;
  size_t moov = b.begin("moov");
  size_t mvhd = b.begin("mvhd");
  b.u32(0), b.u32(0), b.u32(0), b.u32(TIMESCALE), b.u32(duration);
  b.u32(0x10000), b.u16(0x100);  // 재생 속도 1, 음량 1
  b.zeros(10);
  matrix(b, 0, disp_w, disp_h);
  b.zeros(24);
  b.u32(2);  // 다음 트랙 번호
  b.end(mvhd);

  size_t trak = b.begin("trak");
  size_t tkhd = b.begin("tkhd");
  b.u32(3);  // 버전 0, 켜짐 | 영상에 포함
  b.u32(0), b.u32(0), b.u32(1), b.u32(0);
  b.u32(duration);
  b.zeros(8);
  b.u16(0), b.u16(0), b.u16(0), b.u16(0);
  matrix(b, turns, width, height);
  b.u32((uint32_t)width << 16), b.u32((uint32_t)height << 16);
  b.end(tkhd);

  size_t mdia = b.begin("mdia");
  size_t mdhd = b.begin("mdhd");
  b.u32(0), b.u32(0), b.u32(0), b.u32(TIMESCALE), b.u32(duration), b.u16(0), b.u16(0);
  b.end(mdhd);
  size_t hdlr = b.begin("hdlr");
  b.u32(0), b.bytes("mhlr", 4), b.bytes("vide", 4), b.zeros(12), b.u8(0);
  b.end(hdlr);

  size_t minf = b.begin("minf");
  size_t vmhd = b.begin("vmhd");
  b.u32(1), b.u16(0), b.u16(0), b.u16(0), b.u16(0);
  b.end(vmhd);
  size_t dinf = b.begin("dinf");
  size_t dref = b.begin("dref");
  b.u32(0), b.u32(1);
  size_t alis = b.begin("alis");
  b.u32(1);
  b.end(alis);
  b.end(dref);
  b.end(dinf);

  size_t stbl = b.begin("stbl");
  size_t stsd = b.begin("stsd");
  b.u32(0), b.u32(1);
  size_t entry = b.begin("jpeg");
  b.zeros(6), b.u16(1);                    // 예약, 데이터 참조 번호
  b.u16(0), b.u16(0), b.bytes("appl", 4);  // 버전, 개정, 제조사
  b.u32(0), b.u32(512);                    // 시간 품질, 공간 품질
  b.u16(width), b.u16(height);
  b.u32(72 << 16), b.u32(72 << 16);        // 해상도 72dpi
  b.u32(0), b.u16(1);                      // 데이터 크기, 장 수
  char codec[32] = {12, 'P', 'h', 'o', 't', 'o', ' ', '-', ' ', 'J', 'P', 'E', 'G'};
  b.bytes(codec, 32);
  b.u16(24), b.u16(0xFFFF);                // 색 깊이, 색 표 없음
  b.end(entry);
  b.end(stsd);
  size_t stts_box = b.begin("stts");
  b.bytes(stts.p, stts.len);
  b.end(stts_box);
  size_t stsc = b.begin("stsc");
  b.u32(0), b.u32(1), b.u32(1), b.u32(1), b.u32(1);
  b.end(stsc);
  size_t stsz = b.begin("stsz");
  b.u32(0), b.u32(0), b.u32(n);
  for (uint32_t i = 0; i < n; i++) b.u32(sizes[i]);
  b.end(stsz);
  // FAT32 는 파일이 4GB 를 넘을 수 없어서 32비트 위치(stco)로 충분하다
  size_t stco = b.begin("stco");
  b.u32(0), b.u32(n);
  for (uint32_t i = 0; i < n; i++) b.u32(offsets[i]);
  b.end(stco);
  b.end(stbl);
  b.end(minf);
  b.end(mdia);
  b.end(trak);
  b.end(moov);

  file.write(b.p, b.len);
  free(b.p);
  free(stts.p);
}

// ---- 녹화 ----

static void reset_index() {
  free(offsets), free(sizes), free(times);
  offsets = sizes = times = nullptr;
  count = capacity = 0;
}

bool sd_video_start(const char *name, uint8_t t) {
  if (writing || !storage_ready()) return false;
  sd_video_loop();  // 쓰는 태스크를 처음 한 번 만든다
  if (!SD.exists("/videos")) SD.mkdir("/videos");
  snprintf(path, sizeof(path), "/videos/%s", name);
  file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("[sdvideo] cannot create %s\n", path);
    return false;
  }
  Buf head;
  size_t ftyp = head.begin("ftyp");
  head.bytes("qt  ", 4), head.u32(0x20050300), head.bytes("qt  ", 4);
  head.end(ftyp);
  file.write(head.p, head.len);
  free(head.p);
  mdat_at = file.position();
  const uint8_t mdat[8] = {0, 0, 0, 0, 'm', 'd', 'a', 't'};
  file.write(mdat, 8);
  reset_index();
  turns = t;
  width = height = 0;
  dropped = 0;
  failed = false;
  writing = true;
  active = true;
  Serial.printf("[sdvideo] recording to %s\n", path);
  return true;
}

void sd_video_frame(const uint8_t *jpg, size_t len, uint32_t ms) {
  if (!active || failed) return;
  if (uxQueueSpacesAvailable(queue) <= 1) {
    dropped++;
    return;
  }
  uint8_t *copy = (uint8_t *)ps_malloc(len);
  if (!copy) {
    dropped++;
    return;
  }
  memcpy(copy, jpg, len);
  Frame f = {copy, len, ms};
  if (xQueueSend(queue, &f, 0) != pdTRUE) {
    free(copy);
    dropped++;
  }
}

void sd_video_stop() {
  if (!active) return;
  active = false;
  // 끝 표시. 쓰는 태스크가 앞의 장을 다 쓰고 파일을 닫는다.
  Frame end = {nullptr, 0, 0};
  xQueueSend(queue, &end, portMAX_DELAY);
}

static void finish() {
  uint32_t end = file.position();
  file.seek(mdat_at);
  uint32_t size = end - mdat_at;
  uint8_t be[4] = {(uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size};
  file.write(be, 4);
  file.seek(end);
  if (count) write_moov();
  file.close();
  if (!count) SD.remove(path);
  result_frames = count;
  result_dropped = dropped;
  result_failed = failed;
  Serial.printf("[sdvideo] saved %s: %lu frames, %lu dropped%s\n", path, (unsigned long)count,
                (unsigned long)dropped, failed ? ", write failed" : "");
  reset_index();
  writing = false;
}

static void write_frame(Frame &f) {
  if (failed) return;
  if (count == capacity) {
    uint32_t cap = capacity ? capacity * 2 : 1024;
    offsets = (uint32_t *)ps_realloc(offsets, cap * 4);
    sizes = (uint32_t *)ps_realloc(sizes, cap * 4);
    times = (uint32_t *)ps_realloc(times, cap * 4);
    capacity = cap;
  }
  if (!width) jpeg_size(f.data, f.len, &width, &height);
  uint32_t at = file.position();
  if (file.write(f.data, f.len) == f.len && offsets && sizes && times) {
    offsets[count] = at;
    sizes[count] = f.len;
    times[count] = f.ms;
    count++;
  } else {
    // 카드가 꽉 찼거나 빠졌다. 지금까지 쓴 것만 남긴다.
    failed = true;
    Serial.println("[sdvideo] write failed (card full or removed?)");
  }
}

// 카드에 쓰는 일은 한 장에 수십 ms 걸려서, 화면(loop)을 막지 않도록 따로 돌린다.
// 화면과 SPI 선을 같이 쓰지만 SPIClass 잠금으로 번갈아 쓴다 (display.cpp).
static void writer_task(void *) {
  Frame f;
  for (;;) {
    if (xQueueReceive(queue, &f, portMAX_DELAY) != pdTRUE) continue;
    if (!f.data) {
      finish();
      continue;
    }
    write_frame(f);
    free(f.data);
  }
}

void sd_video_loop() {
  static bool started = false;
  if (started) return;
  started = true;
  if (!queue) queue = xQueueCreate(QUEUE_FRAMES + 1, sizeof(Frame));
  xTaskCreatePinnedToCoreWithCaps(writer_task, "sdvideo", 6144, NULL, 1, NULL, 1, MALLOC_CAP_SPIRAM);
}

bool sd_video_active() {
  return writing;
}

void sd_video_result(uint32_t *frames, uint32_t *d, bool *f) {
  *frames = result_frames;
  *d = result_dropped;
  *f = result_failed;
}
