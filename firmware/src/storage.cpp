#include "storage.h"

#include <SD.h>
#include <vector>

#include "display.h"
#include "pins.h"

static bool mounted = false;
static uint32_t tried_ms = 0;

bool storage_ready() {
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
    SD.end();
    mounted = false;
    tried_ms = 0;
    return false;
  }
  Serial.printf("[sd] saved %s (%u bytes)\n", path, (unsigned)len);
  return true;
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
    if (!f.isDirectory()) names.push_back(String(dir) + "/" + f.name());
    f.close();
  }
  d.close();
  int n = 0;
  for (const String &path : names) {
    if (SD.remove(path)) n++;
  }
  Serial.printf("[sd] removed %d files in %s\n", n, dir);
  return n;
}

bool storage_recheck() {
  if (mounted) {
    // 카드를 뺐다 꽂았을 수 있으니 처음부터
    SD.end();
    mounted = false;
  }
  tried_ms = 0;
  return storage_ready();
}

bool storage_format() {
  // 읽을 수 없는 카드(exFAT 등, 64GB 이상 카드의 기본 형식)만 FAT32 로 포맷한다. 이미 읽히는 카드는 건드리지 않는다.
  if (mounted) return true;
  bool ok = SD.begin(SD_CS, display_spi(), 20000000, "/sd", 5, true);
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
