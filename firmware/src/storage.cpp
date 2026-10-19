#include "storage.h"

#include <SD.h>
#include <algorithm>
#include <vector>

#include "display.h"
#include "health.h"
#include "photo.h"
#include "pins.h"
#include "sd_video.h"
#include "ui.h"

static bool mounted = false;
static uint32_t tried_ms = 0;
static bool drop_later = false;  // 동영상을 쓰는 중이라 연결을 끊지 못하고 미뤄 두었다
// 남은 공간 (바이트). FAT32 는 처음 셀 때 오래 걸릴 수 있어 한 번 세고, 쓸 때마다 대충 뺀다. -1 이면 다시 센다.
static volatile int64_t free_bytes = -1;
static uint64_t total_bytes = 0;

// 카드 파일을 열어 쓰거나 읽는 중이라 연결을 끊으면 안 되는지 (동영상 쓰기, PC 로 보내기)
static bool card_busy() {
  return sd_video_active() || photo_streaming();
}

// 연결을 끊고 다음에 처음부터 다시 연결한다. 동영상을 마저 쓰는 중이면(녹화를 멈춘 뒤에도 잠시 쓴다)
// 그 파일이 망가지지 않도록 다 쓸 때까지 미룬다.
static void drop_card() {
  if (!mounted) return;
  if (card_busy()) {
    drop_later = true;
    return;
  }
  SD.end();
  mounted = false;
  drop_later = false;
  tried_ms = 0;
  free_bytes = -1;
}

bool storage_ready() {
  if (drop_later && !card_busy()) drop_card();
  if (mounted) return true;
  // 카드가 없을 때 매번 시도하면 느려지므로 10초에 한 번만
  if (tried_ms != 0 && millis() - tried_ms < 10000) return false;
  tried_ms = millis();
  // LCD 와 같은 SPI 선을 같이 쓴다 (CS 만 다름). 화면 그리기와 같은 태스크(loop)에서만 부른다.
  mounted = SD.begin(SD_CS, display_spi(), 20000000);
  if (mounted) {
    Serial.printf("[sd] card %llu MB\n", SD.cardSize() / (1024 * 1024));
  } else {
    Serial.println("[sd] no card");
  }
  return mounted;
}

bool storage_write(const char *path, const uint8_t *data, size_t len) {
  if (!storage_ready()) return false;
  String dir = String(path).substring(0, String(path).lastIndexOf('/'));
  if (dir.length() > 0 && !SD.exists(dir)) SD.mkdir(dir);
  File f = SD.open(path, FILE_WRITE);
  size_t written = f ? f.write(data, len) : 0;
  if (f) f.close();
  if (written != len) {
    // 카드를 뺐거나 꽉 찼다. 다음에 처음부터 다시 연결한다.
    Serial.printf("[sd] write failed %s (%u/%u)\n", path, (unsigned)written, (unsigned)len);
    drop_card();
    return false;
  }
  storage_note_written(len);
  Serial.printf("[sd] saved %s (%u bytes)\n", path, (unsigned)len);
  return true;
}

int64_t storage_free_bytes() {
  if (!storage_ready()) return -1;
  if (free_bytes < 0) {
    // 카드의 빈칸 정보(FSINFO)가 없으면 FAT 전체를 읽느라 수 초 걸린다
    health_watch_pause(true);
    uint32_t start = millis();
    total_bytes = SD.totalBytes();
    uint64_t used = SD.usedBytes();
    health_watch_pause(false);
    free_bytes = total_bytes > used ? (int64_t)(total_bytes - used) : 0;
    Serial.printf("[sd] free %lld MB of %llu MB (%lu ms)\n", free_bytes / (1024 * 1024), total_bytes / (1024 * 1024),
                  millis() - start);
  }
  return free_bytes;
}

void storage_note_written(size_t len) {
  int64_t f = free_bytes;
  if (f >= 0) free_bytes = f > (int64_t)len ? f - (int64_t)len : 0;
}

void storage_note_removed() {
  free_bytes = -1;  // 처음 한 번 센 뒤로는 빠르다
}

bool storage_space_low(uint32_t *free_mb) {
  int64_t f = storage_free_bytes();
  if (f < 0) return false;
  if (free_mb) *free_mb = f / (1024 * 1024);
  return f < (int64_t)STORAGE_LOW_BYTES || (total_bytes && (uint64_t)f < total_bytes / 20);
}

void storage_warn_low(bool force) {
  static uint32_t warned_ms = 0;
  uint32_t mb;
  if (!storage_space_low(&mb)) return;
  if (!force && warned_ms && millis() - warned_ms < 60000) return;  // 자주 띄우지 않는다
  warned_ms = millis() ? millis() : 1;
  char t[80];
  snprintf(t, sizeof(t), "SD 카드 공간이 얼마 남지 않았습니다 (%lu MB)", (unsigned long)mb);
  ui_toast(t, 4000);
}

bool storage_info(uint32_t *total_mb, uint32_t *used_mb) {
  if (!storage_ready()) return false;
  *total_mb = SD.totalBytes() / (1024 * 1024);
  *used_mb = SD.usedBytes() / (1024 * 1024);
  return true;
}

int storage_count(const char *dir) {
  if (!storage_ready()) return -1;
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) return 0;
  int n = 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (!f.isDirectory()) n++;
    f.close();
  }
  d.close();
  return n;
}

int storage_clear(const char *dir) {
  if (!storage_ready()) return 0;
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) return 0;
  // 지우면서 목록을 읽으면 건너뛰는 파일이 생길 수 있어, 이름을 먼저 모은 뒤 지운다
  std::vector<String> names;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (!f.isDirectory()) names.push_back(f.name());
    f.close();
  }
  d.close();
  std::sort(names.begin(), names.end());
  // 동영상을 마저 쓰는 중이면 그 파일(이름이 가장 뒤인 것)은 남긴다. 열려 있는 파일을 지우면 카드가 망가진다.
  if (sd_video_active() && strcmp(dir, "/videos") == 0 && !names.empty()) names.pop_back();
  int n = 0;
  std::vector<String> removed;
  for (const String &name : names) {
    if (SD.remove(String(dir) + "/" + name)) n++, removed.push_back(name + ".rgb");
    health_watch_feed();  // 파일이 많으면 오래 걸린다
  }
  // 지운 것의 작은 그림(갤러리 격자)도 지운다
  File t = SD.open(STORAGE_THUMBS);
  if (t && t.isDirectory() && !removed.empty()) {
    std::vector<String> thumbs;
    for (File f = t.openNextFile(); f; f = t.openNextFile()) {
      if (!f.isDirectory() && std::binary_search(removed.begin(), removed.end(), String(f.name()))) thumbs.push_back(f.name());
      f.close();
    }
    for (const String &name : thumbs) {
      SD.remove(String(STORAGE_THUMBS) + "/" + name);
      health_watch_feed();
    }
  }
  if (t) t.close();
  if (n) storage_note_removed();
  Serial.printf("[sd] removed %d files in %s\n", n, dir);
  return n;
}

bool storage_present() {
  // 연결된 뒤에 카드를 뺐는지는 storage_ready 만으로는 모른다. 첫 칸(512바이트)을 읽어 본다.
  // 동영상을 쓰는 중에는 건드리지 않는다 (빠졌으면 그쪽이 알아서 실패한다).
  if (mounted && !sd_video_active()) {
    static uint8_t sector[512];
    if (!SD.readRAW(sector, 0)) {
      Serial.println("[sd] card removed");
      drop_card();
    }
  }
  if (!mounted) tried_ms = 0;  // 방금 꽂았을 수 있으니 10초 기다리지 않고 바로
  return storage_ready();
}

String storage_thumb_path(const String &path) {
  return String(STORAGE_THUMBS) + "/" + path.substring(path.lastIndexOf('/') + 1) + ".rgb";
}

bool storage_recheck() {
  // 카드를 뺐다 꽂았을 수 있으니 처음부터 (동영상을 마저 쓰는 중이면 그대로 둔다)
  if (card_busy()) return mounted;
  drop_card();
  tried_ms = 0;
  return storage_ready();
}

bool storage_format() {
  // 읽을 수 없는 카드(exFAT 등, 64GB 이상 카드의 기본 형식)만 FAT32 로 포맷한다. 이미 읽히는 카드는 건드리지 않는다.
  if (mounted) return true;
  // 큰 카드는 포맷이 감시 장치의 한계시간보다 오래 걸릴 수 있다
  health_watch_pause(true);
  bool ok = SD.begin(SD_CS, display_spi(), 20000000, "/sd", 5, true);
  free_bytes = -1;
  health_watch_pause(false);
  if (ok) {
    mounted = true;
    Serial.printf("[sd] formatted, %llu MB\n", SD.totalBytes() / (1024 * 1024));
  } else {
    Serial.println("[sd] format failed (no card?)");
  }
  tried_ms = millis();
  return ok;
}

bool storage_read_last(const char *dir, char *name, size_t name_size, uint8_t **data, size_t *len) {
  if (!storage_ready()) return false;
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) return false;
  String last;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (!f.isDirectory() && String(f.name()) > last) last = f.name();
    f.close();
  }
  d.close();
  if (!last.length()) return false;
  File f = SD.open(String(dir) + "/" + last);
  if (!f) return false;
  *len = f.size();
  *data = (uint8_t *)ps_malloc(*len);
  bool ok = *data && f.read(*data, *len) == *len;
  f.close();
  if (!ok) {
    free(*data);
    return false;
  }
  strlcpy(name, last.c_str(), name_size);
  return true;
}
