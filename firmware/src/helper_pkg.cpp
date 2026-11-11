#include "helper_pkg.h"

#include <SD.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

#include "sd_video.h"
#include "storage.h"
#include "usb_link.h"

static void reply(JsonDocument &doc) {
  char out[256];
  serializeJson(doc, out, sizeof(out));
  usb_send_event(out);
}

static void reply_error(const char *name, const char *error) {
  JsonDocument doc;
  doc["pkg"]["name"] = name;
  doc["pkg"]["error"] = error;
  reply(doc);
  Serial.printf("[pkg] %s: %s\n", name, error);
}

static void reply_version() {
  JsonDocument doc;
  bool card = storage_ready();
  doc["pkg"]["card"] = card;
  char ver[64] = "";
  File f = card ? SD.open(HELPER_PKG_DIR "/version.txt") : File();
  if (f) {
    size_t n = f.read((uint8_t *)ver, sizeof(ver) - 1);
    ver[n] = 0;
    f.close();
    for (char *p = ver; *p; p++) if (*p == '\r' || *p == '\n') { *p = 0; break; }
  }
  doc["pkg"]["ver"] = ver;
  reply(doc);
}

void helper_pkg_command(JsonObjectConst pkg) {
  if (pkg["ask"] == true) {
    reply_version();
    return;
  }
  const char *name = pkg["name"] | "";
  if (!helper_pkg_name_ok(name)) return reply_error(name, "bad name");
  if (!storage_ready()) return reply_error(name, "no card");
  if (sd_video_active()) return reply_error(name, "busy");  // 녹화가 다른 태스크에서 카드에 쓰는 중
  char part[80], final_path[80];
  snprintf(final_path, sizeof(final_path), HELPER_PKG_DIR "/%s", name);
  snprintf(part, sizeof(part), "%s.part", final_path);
  if (!SD.exists(HELPER_PKG_DIR)) SD.mkdir(HELPER_PKG_DIR);
  uint32_t off = pkg["off"] | 0;
  size_t have = 0;
  if (off > 0) {
    File f = SD.open(part);
    have = f ? f.size() : 0;
    if (f) f.close();
  }
  if (off == have) {
    static uint8_t *buf = (uint8_t *)heap_caps_malloc(HELPER_PKG_CHUNK, MALLOC_CAP_SPIRAM);
    const char *b64 = pkg["data"] | "";
    size_t len = 0;
    if (!buf || mbedtls_base64_decode(buf, HELPER_PKG_CHUNK, &len, (const unsigned char *)b64, strlen(b64)) != 0)
      return reply_error(name, "bad data");
    File f = SD.open(part, off == 0 ? FILE_WRITE : FILE_APPEND);
    size_t wrote = f ? f.write(buf, len) : 0;
    if (f) f.close();
    if (wrote != len) return reply_error(name, "write failed");
    storage_note_written(len);
    have = off + len;
    if (pkg["done"] == true) {
      SD.remove(final_path);
      if (!SD.rename(part, final_path)) return reply_error(name, "rename failed");
      Serial.printf("[pkg] saved %s (%u bytes)\n", final_path, (unsigned)have);
    }
  }
  JsonDocument doc;
  doc["pkg"]["name"] = name;
  doc["pkg"]["off"] = have;
  reply(doc);
}
