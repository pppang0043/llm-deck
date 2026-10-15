#include "video.h"

#include <WiFi.h>
#include <freertos/idf_additions.h>
#include <time.h>

#include "link.h"
#include "mic.h"
#include "usb_link.h"

enum JobKind { JOB_START, JOB_FRAME, JOB_END };

struct Job {
  JobKind kind;
  uint8_t *data;  // FRAME: JPEG, START/END: 알림 JSON
  size_t len;
  uint32_t no;
  uint32_t ms;
};

// 보내기를 기다리는 장 수. 넘치면 그 장은 버린다 (전송이 느릴 때 녹화가 밀리지 않도록).
#define QUEUE_FRAMES 6

static QueueHandle_t jobs;
static volatile bool recording = false;
static Link link_used = LINK_NONE;
static uint32_t start_ms = 0;
static uint32_t frame_no = 0;
static uint32_t dropped = 0;
static char name[40];
static WiFiClient tcp;

static void write_frame(uint8_t kind, const uint8_t *head, size_t head_len, const uint8_t *data, size_t len) {
  if (link_used == LINK_USB) {
    usb_send_frame(kind, head, head_len, data, len);
    return;
  }
  if (!tcp.connected()) return;
  uint8_t frame_head[5] = {0xA5, 0x5A, kind, (uint8_t)((head_len + len) & 0xFF), (uint8_t)((head_len + len) >> 8)};
  tcp.write(frame_head, 5);
  if (head_len) tcp.write(head, head_len);
  if (len) tcp.write(data, len);
}

static void send_event(const char *json) {
  write_frame(USB_FRAME_EVENT, nullptr, 0, (const uint8_t *)json, strlen(json));
}

static void sender_task(void *) {
  Job job;
  for (;;) {
    if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
    if (job.kind == JOB_START && link_used == LINK_WIFI) {
      tcp.stop();
      if (!tcp.connect(mic_target(), VIDEO_TCP_PORT, 3000)) {
        Serial.println("[video] cannot reach the PC over TCP");
      }
    }
    if (job.kind == JOB_FRAME && link_used == LINK_WIFI) {
      // Wi-Fi 는 작은 조각을 따로 보내면 느리므로, 한 장의 조각들을 이어 붙여 한 번에 보낸다
      size_t chunks = (job.len + 1023) / 1024;
      size_t total = job.len + chunks * (5 + 16);
      uint8_t *buf = (uint8_t *)ps_malloc(total);
      if (buf) {
        size_t pos = 0;
        for (size_t off = 0; off < job.len; off += 1024) {
          size_t n = min((size_t)1024, job.len - off);
          uint32_t head[4] = {job.no, (uint32_t)off, (uint32_t)job.len, job.ms};
          uint8_t frame_head[5] = {0xA5, 0x5A, USB_FRAME_VIDEO, (uint8_t)((16 + n) & 0xFF), (uint8_t)((16 + n) >> 8)};
          memcpy(buf + pos, frame_head, 5);
          memcpy(buf + pos + 5, head, 16);
          memcpy(buf + pos + 21, job.data + off, n);
          pos += 21 + n;
        }
        if (tcp.connected()) tcp.write(buf, pos);
        free(buf);
      }
    } else if (job.kind == JOB_FRAME) {
      // 1KB 조각으로 나눠 보낸다. 각 조각에 장 번호·위치·크기·시각을 붙인다.
      for (size_t off = 0; off < job.len; off += 1024) {
        size_t n = min((size_t)1024, job.len - off);
        uint32_t head[4] = {job.no, (uint32_t)off, (uint32_t)job.len, job.ms};
        write_frame(USB_FRAME_VIDEO, (const uint8_t *)head, sizeof(head), job.data + off, n);
      }
    } else {
      send_event((const char *)job.data);
    }
    if (job.kind == JOB_END) {
      Serial.printf("[video] %s\n", (const char *)job.data);
      if (link_used == LINK_WIFI) {
        tcp.flush();
        tcp.stop();
      }
    }
    free(job.data);
  }
}

void video_init() {
  jobs = xQueueCreate(QUEUE_FRAMES + 2, sizeof(Job));
  xTaskCreatePinnedToCoreWithCaps(sender_task, "video", 6144, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
}

static bool enqueue_text(JobKind kind, const char *text) {
  Job job = {kind, (uint8_t *)strdup(text), strlen(text), 0, 0};
  if (!job.data) return false;
  if (xQueueSend(jobs, &job, pdMS_TO_TICKS(500)) != pdTRUE) {
    free(job.data);
    return false;
  }
  return true;
}

const char *video_start(uint8_t turns) {
  if (recording) return nullptr;
  Link link = link_active();
  if (link == LINK_BLE) return "블루투스로는 동영상을\n보낼 수 없습니다\nUSB나 Wi-Fi로 연결하세요";
  if (link != LINK_USB && link != LINK_WIFI) return "PC가 연결되어 있지 않습니다\nUSB나 Wi-Fi로 연결하세요";

  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm t;
    localtime_r(&now, &t);
    strftime(name, sizeof(name), "VID_%Y%m%d_%H%M%S.mov", &t);
  } else {
    snprintf(name, sizeof(name), "VID_%lu.mov", (unsigned long)millis());
  }
  link_used = link;
  frame_no = 0;
  dropped = 0;
  start_ms = millis();
  char event[100];
  snprintf(event, sizeof(event), "{\"video\":{\"start\":\"%s\",\"turns\":%d}}", name, turns);
  if (!enqueue_text(JOB_START, event)) return "녹화를 시작하지 못했습니다";
  recording = true;
  Serial.printf("[video] start %s via %s\n", name, link == LINK_USB ? "USB" : "Wi-Fi");
  return nullptr;
}

void video_frame(const uint8_t *jpg, size_t len) {
  if (!recording) return;
  uint32_t ms = millis() - start_ms;
  if (uxQueueSpacesAvailable(jobs) <= 2) {
    dropped++;
    return;
  }
  uint8_t *copy = (uint8_t *)ps_malloc(len);
  if (!copy) {
    dropped++;
    return;
  }
  memcpy(copy, jpg, len);
  Job job = {JOB_FRAME, copy, len, frame_no++, ms};
  if (xQueueSend(jobs, &job, 0) != pdTRUE) {
    free(copy);
    frame_no--;
    dropped++;
  }
}

const char *video_stop() {
  if (!recording) return nullptr;
  recording = false;
  uint32_t seconds = (millis() - start_ms) / 1000;
  char event[120];
  snprintf(event, sizeof(event), "{\"video\":{\"end\":\"%s\",\"frames\":%lu,\"dropped\":%lu}}", name,
           (unsigned long)frame_no, (unsigned long)dropped);
  enqueue_text(JOB_END, event);
  static char result[64];
  snprintf(result, sizeof(result), "PC로 저장했습니다\n%lu초, %lu장", (unsigned long)seconds, (unsigned long)frame_no);
  return result;
}

bool video_recording() {
  return recording;
}

uint32_t video_elapsed_ms() {
  return recording ? millis() - start_ms : 0;
}
