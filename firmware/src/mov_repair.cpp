#include "mov_repair.h"

#include <SD.h>

#include "health.h"

#define TIMESCALE 1000
#define FRAME_MS 100  // 시각이 남아 있지 않아 1초에 10장으로 본다
#define CHUNK (32 * 1024)
// 한 장이 이보다 크면 깨진 것으로 보고 다음 시작을 찾는다
#define FRAME_MAX (2 * 1024 * 1024)

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// ---- 큰 끝(big-endian) 상자 쓰기 (sd_video.cpp 와 같은 모양) ----

struct Buf {
  uint8_t *p = nullptr;
  size_t len = 0, cap = 0;
  bool bad = false;
  void need(size_t n) {
    if (bad || len + n <= cap) return;
    size_t c = max(cap * 2, len + n + 1024);
    uint8_t *q = (uint8_t *)ps_realloc(p, c);
    if (!q) {
      bad = true;
      return;
    }
    p = q, cap = c;
  }
  void u8(uint8_t v) {
    need(1);
    if (!bad) p[len++] = v;
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
    if (bad) return;
    memcpy(p + len, d, n);
    len += n;
  }
  void zeros(size_t n) {
    need(n);
    if (bad) return;
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
    if (bad) return;
    uint32_t size = len - at;
    p[at] = size >> 24, p[at + 1] = size >> 16, p[at + 2] = size >> 8, p[at + 3] = size;
  }
};

// 돌리지 않은 변환 행렬
static void identity(Buf &b) {
  b.u32(0x10000), b.u32(0), b.u32(0);
  b.u32(0), b.u32(0x10000), b.u32(0);
  b.u32(0), b.u32(0), b.u32(0x40000000);
}

// sd_video.cpp 의 write_moov 와 같은 목차 (장마다 같은 시간)
static void build_moov(Buf &b, const uint32_t *offsets, const uint32_t *sizes, uint32_t n, uint16_t w, uint16_t h) {
  uint32_t duration = n * FRAME_MS;
  size_t moov = b.begin("moov");
  size_t mvhd = b.begin("mvhd");
  b.u32(0), b.u32(0), b.u32(0), b.u32(TIMESCALE), b.u32(duration);
  b.u32(0x10000), b.u16(0x100);
  b.zeros(10);
  identity(b);
  b.zeros(24);
  b.u32(2);
  b.end(mvhd);

  size_t trak = b.begin("trak");
  size_t tkhd = b.begin("tkhd");
  b.u32(3);
  b.u32(0), b.u32(0), b.u32(1), b.u32(0);
  b.u32(duration);
  b.zeros(8);
  b.u16(0), b.u16(0), b.u16(0), b.u16(0);
  identity(b);
  b.u32((uint32_t)w << 16), b.u32((uint32_t)h << 16);
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
  b.zeros(6), b.u16(1);
  b.u16(0), b.u16(0), b.bytes("appl", 4);
  b.u32(0), b.u32(512);
  b.u16(w), b.u16(h);
  b.u32(72 << 16), b.u32(72 << 16);
  b.u32(0), b.u16(1);
  char codec[32] = {12, 'P', 'h', 'o', 't', 'o', ' ', '-', ' ', 'J', 'P', 'E', 'G'};
  b.bytes(codec, 32);
  b.u16(24), b.u16(0xFFFF);
  b.end(entry);
  b.end(stsd);
  size_t stts = b.begin("stts");
  b.u32(0), b.u32(1), b.u32(n), b.u32(FRAME_MS);
  b.end(stts);
  size_t stsc = b.begin("stsc");
  b.u32(0), b.u32(1), b.u32(1), b.u32(1), b.u32(1);
  b.end(stsc);
  size_t stsz = b.begin("stsz");
  b.u32(0), b.u32(0), b.u32(n);
  for (uint32_t i = 0; i < n; i++) b.u32(sizes[i]);
  b.end(stsz);
  size_t stco = b.begin("stco");
  b.u32(0), b.u32(n);
  for (uint32_t i = 0; i < n; i++) b.u32(offsets[i]);
  b.end(stco);
  b.end(stbl);
  b.end(minf);
  b.end(mdia);
  b.end(trak);
  b.end(moov);
}

// ---- JPEG 경계 찾기 ----
// 머리 부분은 표시(FF xx)와 길이로 건너뛰고, 그림 데이터 안에서는 FF 뒤에 00/RST 가 아닌 것이 오면 표시로 본다.
// (그림 데이터 안의 FF 는 항상 FF00 으로 적혀서, FFD9 는 진짜 끝에서만 나온다)

enum State { SEEK_SOI, MARKER, MARKER_TYPE, LEN_HI, LEN_LO, SKIP, ENTROPY, ENTROPY_FF };

struct Scanner {
  State st = SEEK_SOI;
  uint8_t prev = 0;
  bool sos = false;  // 지금 건너뛰는 머리가 SOS 다 (끝나면 그림 데이터)
  uint32_t start = 0, skip = 0;
  uint16_t seg_len = 0;
  uint32_t *offsets = nullptr, *sizes = nullptr;
  uint32_t count = 0, cap = 0;
  bool oom = false;

  void found(uint32_t end) {
    if (count == cap) {
      uint32_t c = cap ? cap * 2 : 1024;
      uint32_t *o = (uint32_t *)ps_realloc(offsets, c * 4);
      if (o) offsets = o;
      uint32_t *s = (uint32_t *)ps_realloc(sizes, c * 4);
      if (s) sizes = s;
      if (!o || !s) {
        oom = true;
        return;
      }
      cap = c;
    }
    offsets[count] = start;
    sizes[count] = end - start;
    count++;
  }

  void marker(uint8_t b, uint32_t at) {
    if (b == 0xFF) return;  // 채움
    if (b == 0xD9) {
      found(at + 1);
      st = SEEK_SOI;
    } else if (b == 0x01 || (b >= 0xD0 && b <= 0xD7)) {
      st = MARKER;  // 길이 없는 표시
    } else if (b == 0xD8) {
      start = at - 1;  // 끝 없이 새 장이 시작됐다
      st = MARKER;
    } else {
      sos = b == 0xDA;
      st = LEN_HI;
    }
  }

  // data 는 파일의 at 위치부터 n 바이트
  void feed(const uint8_t *data, size_t n, uint32_t at) {
    for (size_t i = 0; i < n && !oom; i++) {
      uint8_t b = data[i];
      uint32_t pos = at + i;
      if (st != SEEK_SOI && pos - start > FRAME_MAX) st = SEEK_SOI, prev = 0;
      switch (st) {
        case SEEK_SOI:
          if (prev == 0xFF && b == 0xD8) start = pos - 1, st = MARKER;
          break;
        case MARKER:
          st = b == 0xFF ? MARKER_TYPE : SEEK_SOI;
          break;
        case MARKER_TYPE:
          marker(b, pos);
          break;
        case LEN_HI:
          seg_len = b << 8;
          st = LEN_LO;
          break;
        case LEN_LO:
          seg_len |= b;
          if (seg_len < 2) {
            st = SEEK_SOI;
          } else if (seg_len == 2) {
            st = sos ? ENTROPY : MARKER;
          } else {
            skip = seg_len - 2;
            st = SKIP;
          }
          break;
        case SKIP: {
          // 한꺼번에 건너뛴다
          size_t take = min((size_t)skip, n - i);
          i += take - 1;
          skip -= take;
          if (!skip) st = sos ? ENTROPY : MARKER;
          break;
        }
        case ENTROPY: {
          const uint8_t *ff = (const uint8_t *)memchr(data + i, 0xFF, n - i);
          if (!ff) {
            i = n - 1;
          } else {
            i = ff - data;
            st = ENTROPY_FF;
          }
          break;
        }
        case ENTROPY_FF:
          if (b == 0x00 || (b >= 0xD0 && b <= 0xD7)) {
            st = ENTROPY;
          } else if (b != 0xFF) {
            sos = false;
            marker(b, pos);  // 끝(D9) 또는 다음 머리 (스캔이 여러 개인 JPEG)
          }
          break;
      }
      prev = data[i];
    }
  }
};

// 첫 장 머리(SOF)에서 가로·세로
static void frame_size(File &f, uint32_t at, uint32_t len, uint16_t *w, uint16_t *h) {
  *w = 640, *h = 480;
  uint8_t head[1024];
  size_t n = min((uint32_t)sizeof(head), len);
  if (!f.seek(at) || f.read(head, n) != n) return;
  size_t i = 2;
  while (i + 9 < n && head[i] == 0xFF) {
    uint8_t m = head[i + 1];
    if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
      *h = (head[i + 5] << 8) | head[i + 6];
      *w = (head[i + 7] << 8) | head[i + 8];
      return;
    }
    i += 2 + ((head[i + 2] << 8) | head[i + 3]);
  }
}

uint32_t mov_repair(const char *path, void (*progress)(int pct)) {
  File f = SD.open(path, "r");
  if (!f) return 0;
  uint32_t size = f.size();
  // 머리: ftyp 다음 mdat (sd_video_start)
  uint8_t head[16];
  if (size < 16 || f.read(head, 16) != 16 || memcmp(head + 4, "ftyp", 4) != 0) {
    f.close();
    return 0;
  }
  uint32_t mdat_at = be32(head);
  uint8_t box[8];
  if (mdat_at < 8 || mdat_at + 8 > size || !f.seek(mdat_at) || f.read(box, 8) != 8 || memcmp(box + 4, "mdat", 4) != 0) {
    f.close();
    return 0;
  }
  uint32_t data_at = mdat_at + 8;

  uint8_t *chunk = (uint8_t *)ps_malloc(CHUNK);
  Scanner sc;
  bool read_ok = chunk != nullptr;
  uint32_t t0 = millis(), told = 0;
  f.seek(data_at);
  for (uint32_t pos = data_at; read_ok && pos < size && !sc.oom;) {
    size_t n = min((uint32_t)CHUNK, size - pos);
    if (f.read(chunk, n) != n) {
      // 끝 부분을 못 읽으면 거기까지만 쓴다
      Serial.printf("[repair] read failed at %lu\n", (unsigned long)pos);
      break;
    }
    sc.feed(chunk, n, pos);
    pos += n;
    health_watch_feed();  // 큰 파일은 오래 걸린다
    if (progress && millis() - told > 500) {
      told = millis();
      progress((int)((uint64_t)(pos - data_at) * 100 / (size - data_at)));
    }
  }
  free(chunk);
  uint32_t n = sc.oom ? 0 : sc.count;
  Serial.printf("[repair] %s: %lu frames in %lu bytes (%lu ms)\n", path, (unsigned long)n, (unsigned long)size,
                (unsigned long)(millis() - t0));
  uint16_t w = 0, h = 0;
  if (n) frame_size(f, sc.offsets[0], sc.sizes[0], &w, &h);
  f.close();
  if (!read_ok || !n) {
    free(sc.offsets), free(sc.sizes);
    return 0;
  }

  Buf moov;
  build_moov(moov, sc.offsets, sc.sizes, n, w, h);
  free(sc.offsets), free(sc.sizes);
  if (moov.bad) {
    free(moov.p);
    return 0;
  }
  // 목차를 파일 끝에 붙인 뒤에야 mdat 크기를 고친다. 목차를 쓰다 실패해도 mdat 크기가 0(파일 끝까지)이라
  // 장들은 그대로이고, 다음에 다시 고칠 수 있다.
  bool ok = false;
  File w_f = SD.open(path, "r+");
  if (w_f && w_f.seek(size) && w_f.write(moov.p, moov.len) == moov.len) {
    w_f.flush();
    uint32_t mdat_size = size - mdat_at;
    uint8_t be[4] = {(uint8_t)(mdat_size >> 24), (uint8_t)(mdat_size >> 16), (uint8_t)(mdat_size >> 8), (uint8_t)mdat_size};
    ok = w_f.seek(mdat_at) && w_f.write(be, 4) == 4;
  }
  if (w_f) w_f.close();
  free(moov.p);
  Serial.printf("[repair] %s %s\n", path, ok ? "fixed" : "write failed");
  return ok ? n : 0;
}
