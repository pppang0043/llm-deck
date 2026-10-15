#include "storage.h"

#include <SD.h>

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
