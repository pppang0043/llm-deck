#include "photo.h"

#include <Preferences.h>
#include <SD.h>
#include <freertos/idf_additions.h>
#include <time.h>

#include "ble_link.h"
#include "link.h"
#include "settings.h"
#include "storage.h"
#include "usb_link.h"

struct Job {
  uint8_t *data;  // nullptr 이면 path 의 카드 파일을 조금씩 읽어 보낸다
  size_t len;
  char name[40];
  Link link;
  char path[64];
};

// 카드 파일을 이만큼씩 읽어 보낸다 (큰 동영상도 PSRAM 에 다 올리지 않는다)
#define STREAM_BLOCK (16 * 1024)

static QueueHandle_t jobs;
static volatile bool sending = false;
// 화면에 보여줄 진행 상황 (보내는 태스크가 쓰고 화면이 읽는다)
static volatile size_t progress_done = 0;
static volatile size_t progress_total = 0;
static volatile uint32_t finished_count = 0;
static volatile bool finished_ok = false;
static volatile uint32_t finished_ms = 0;
static char finished_name[40] = "";
static char stream_path[64] = "";  // 카드에서 읽어 보내는 중인 파일 (없으면 "")
static Preferences prefs;

static uint8_t *last_data = nullptr;
static size_t last_len = 0;
static char last_name[40] = "";
static SemaphoreHandle_t last_lock;

// PC 가 읽어 가는 만큼만 USB 로 쓴다. 넘치게 쓰면 묶음 중간이 잘려 PC 가 받지 못한다.
static bool wait_usb_room(size_t n) {
  uint32_t start = millis();
  while (Serial.availableForWrite() < (int)n) {
    if (millis() - start > 5000 || link_active() != LINK_USB) return false;
    vTaskDelay(1);
  }
  return true;
}

// 카드의 파일을 STREAM_BLOCK 씩 읽어 USB 로 보낸다. SPI 는 화면과 잠금으로 번갈아 쓴다 (sd_video 와 같이).
static bool stream_file(const Job &job, File &f, uint8_t *block) {
  for (size_t off = 0; off < job.len;) {
    size_t want = min((size_t)STREAM_BLOCK, job.len - off);
    if (f.read(block, want) != want) {
      Serial.printf("[photo] read failed %s at %u\n", job.path, (unsigned)off);
      return false;
    }
    for (size_t i = 0; i < want;) {
      size_t n = min((size_t)1024, want - i);
      if (!wait_usb_room(5 + 4 + n)) return false;
      usb_send_photo_chunk(off + i, block + i, n);
      i += n;
    }
    off += want;
    progress_done = off;
  }
  return true;
}

static void send_job(const Job &job) {
  char event[120];
  if (job.link == LINK_WIFI) {
    snprintf(event, sizeof(event), "{\"photo\":{\"name\":\"%s\",\"size\":%u,\"url\":\"/photo\"}}", job.name,
             (unsigned)job.len);
    link_send_event(event);
    return;
  }
  snprintf(event, sizeof(event), "{\"photo\":{\"name\":\"%s\",\"size\":%u}}", job.name, (unsigned)job.len);
  link_send_event(event);
  uint32_t start = millis();
  bool ok = true;
  progress_total = job.len;
  progress_done = 0;
  if (!job.data) {
    File f = SD.open(job.path);
    uint8_t *block = (uint8_t *)ps_malloc(STREAM_BLOCK);
    ok = f && block && f.size() == job.len && stream_file(job, f, block);
    if (f) f.close();
    free(block);
  }
  for (size_t off = 0; job.data && off < job.len && ok;) {
    if (link_active() != job.link) {
      ok = false;
      break;
    }
    size_t n;
    if (job.link == LINK_USB) {
      n = min((size_t)1024, job.len - off);
      ok = wait_usb_room(5 + 4 + n);
      if (!ok) break;
      usb_send_photo_chunk(off, job.data + off, n);
    } else {
      n = min(ble_photo_chunk_max(), job.len - off);
      ok = n > 0 && ble_send_photo_chunk(off, job.data + off, n);
    }
    off += n;
    progress_done = off;
  }
  finished_ok = ok;
  finished_ms = millis() - start;
  strlcpy(finished_name, job.name, sizeof(finished_name));
  finished_count++;
  Serial.printf("[photo] sent %s to PC: %s (%lu ms)\n", job.name, ok ? "ok" : "interrupted", millis() - start);
}

// PC 로 보내는 일은 오래 걸릴 수 있어(블루투스) 따로 돌린다. 화면은 그동안에도 움직인다.
static void sender_task(void *) {
  Job job;
  for (;;) {
    if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
    sending = true;
    if (!job.data) strlcpy(stream_path, job.path, sizeof(stream_path));
    send_job(job);
    free(job.data);
    if (!job.data) stream_path[0] = 0;
    sending = false;
  }
}

void photo_init() {
  prefs.begin("llm-deck");
  jobs = xQueueCreate(3, sizeof(Job));
  last_lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCoreWithCaps(sender_task, "photo", 4096, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
}

static void make_name(char *out, size_t size) {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm t;
    localtime_r(&now, &t);
    strftime(out, size, "IMG_%Y%m%d_%H%M%S.jpg", &t);
  } else {
    // 아직 시계를 못 맞췄으면 번호로
    uint32_t no = prefs.getUInt("photo_no", 0) + 1;
    prefs.putUInt("photo_no", no);
    snprintf(out, size, "IMG_%05lu.jpg", (unsigned long)no);
  }
}

// 연결된 PC 로 보낸다. jpg 는 여기서 맡는다 (보낸 뒤 또는 못 보내면 해제). 보내는 길을 *link 에.
static bool send_to_pc(const char *name, uint8_t *jpg, size_t len, Link *link_out) {
  // Wi-Fi 로 받아 갈 수 있게 마지막 사진으로 둔다 (원본은 아래에서 PC 전송에 쓴다)
  uint8_t *copy = (uint8_t *)ps_malloc(len);
  if (copy) {
    memcpy(copy, jpg, len);
    xSemaphoreTake(last_lock, portMAX_DELAY);
    free(last_data);
    last_data = copy;
    last_len = len;
    strlcpy(last_name, name, sizeof(last_name));
    xSemaphoreGive(last_lock);
  }

  Link link = link_active();
  *link_out = link;
  bool to_pc = link == LINK_USB || link == LINK_BLE || link == LINK_WIFI;
  if (to_pc) {
    Job job = {jpg, len, "", link};
    strlcpy(job.name, name, sizeof(job.name));
    if (xQueueSend(jobs, &job, 0) != pdTRUE) {
      free(jpg);
      to_pc = false;
    }
  } else {
    free(jpg);
  }
  return to_pc;
}

const char *photo_save(uint8_t *jpg, size_t len, bool to_sd) {
  char name[40];
  make_name(name, sizeof(name));

  char path[56];
  snprintf(path, sizeof(path), "/photos/%s", name);
  bool sd = to_sd && settings_sd_photos() && storage_ready();
  // 이름이 초 단위라 1초 안에 두 장 찍으면 덮어쓴다. 있으면 _2, _3... 을 붙인다 (PC 에도 같은 이름으로).
  if (sd && SD.exists(path)) {
    char base[40];
    strlcpy(base, name, sizeof(base));
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    for (int n = 2; n < 100 && SD.exists(path); n++) {
      snprintf(name, sizeof(name), "%s_%d.jpg", base, n);
      snprintf(path, sizeof(path), "/photos/%s", name);
    }
  }
  // 카드가 가득 찼으면 쓰지 않는다 (쓰다 실패하면 카드를 다시 연결하느라 느려진다)
  int64_t room = sd ? storage_free_bytes() : -1;
  bool full = sd && room >= 0 && room < (int64_t)len + 64 * 1024;
  bool on_sd = sd && !full && storage_write(path, jpg, len);
  Link link;
  bool to_pc = send_to_pc(name, jpg, len, &link);
  Serial.printf("[photo] %s %u bytes, sd %d, pc %d\n", name, (unsigned)len, on_sd, to_pc);
  if (on_sd) storage_warn_low();

  if (full) return to_pc ? "SD 카드가 가득 차 PC로만 보냈습니다" : "SD 카드가 가득 차 저장하지 못했습니다";
  if (on_sd && to_pc) return "저장했습니다: SD 카드 + PC";
  if (on_sd) return "저장했습니다: SD 카드";
  // 블루투스는 수 초 걸린다. 다 보내면 화면에 따로 알린다 (camera_ui).
  if (to_pc) return link == LINK_BLE ? "블루투스로 PC에 보내는 중" : "PC로 보냈습니다";
  return "저장할 곳이 없습니다\nSD 카드를 넣거나 PC를 연결하세요";
}

const char *photo_resend(const char *name, uint8_t *jpg, size_t len) {
  Link link;
  bool to_pc = send_to_pc(name, jpg, len, &link);
  Serial.printf("[photo] resend %s %u bytes, pc %d\n", name, (unsigned)len, to_pc);
  if (!to_pc) return "연결된 PC가 없습니다";
  return link == LINK_BLE ? "블루투스로 PC에 보내는 중" : "PC로 보냈습니다";
}

bool photo_send_file(const char *name, uint8_t *data, size_t len) {
  Link link = link_active();
  if (link != LINK_USB && link != LINK_BLE) {
    free(data);
    return false;
  }
  Job job = {data, len, "", link};
  strlcpy(job.name, name, sizeof(job.name));
  if (xQueueSend(jobs, &job, 0) != pdTRUE) {
    free(data);
    return false;
  }
  return true;
}

bool photo_send_sd_file(const char *path) {
  if (link_active() != LINK_USB) return false;
  File f = SD.open(path);
  if (!f) return false;
  Job job = {nullptr, f.size(), "", LINK_USB, ""};
  f.close();
  if (!job.len) return false;
  const char *slash = strrchr(path, '/');
  strlcpy(job.name, slash ? slash + 1 : path, sizeof(job.name));
  strlcpy(job.path, path, sizeof(job.path));
  // 큐에 넣는 순간부터 보내는 중으로 본다 (그 사이 지우거나 카드 연결을 끊지 않게)
  strlcpy(stream_path, path, sizeof(stream_path));
  if (xQueueSend(jobs, &job, 0) != pdTRUE) {
    stream_path[0] = 0;
    return false;
  }
  return true;
}

bool photo_streaming(const char *path) {
  if (!stream_path[0]) return false;
  return !path || strcmp(path, stream_path) == 0;
}

bool photo_last(const uint8_t **data, size_t *len, const char **name) {
  if (!last_data) return false;
  *data = last_data;
  *len = last_len;
  *name = last_name;
  return true;
}

bool photo_sending() {
  return sending;
}

bool photo_progress(int *pct) {
  if (!sending || !progress_total) return false;
  *pct = progress_done * 100 / progress_total;
  return true;
}

uint32_t photo_finished(bool *ok, uint32_t *ms, const char **name) {
  *ok = finished_ok;
  *ms = finished_ms;
  if (name) *name = finished_name;
  return finished_count;
}
