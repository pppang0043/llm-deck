#include "cloud.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <time.h>

#include "psram_json.h"
#include "camera.h"
#include "link.h"
#include "settings.h"

// 공유 저장소는 최대 5분 늦게 반영되므로 자주 읽을 필요가 없다
#define POLL_MS 60000
#define URL_MAX 200
// 시각을 표시할 때 쓰는 시간대 (한국 표준시)
#ifndef TIMEZONE
#define TIMEZONE "KST-9"
#endif

static Preferences prefs;
static SemaphoreHandle_t lock;
static char url[URL_MAX] = "";

static bool clock_ready() {
  return time(nullptr) > 1700000000;
}

// 오늘이면 시각만, 아니면 날짜까지
static void format_when(time_t when, char *out, size_t len) {
  time_t now = time(nullptr);
  struct tm now_tm, when_tm;
  localtime_r(&now, &now_tm);
  localtime_r(&when, &when_tm);
  bool today = now_tm.tm_year == when_tm.tm_year && now_tm.tm_yday == when_tm.tm_yday;
  if (today) {
    snprintf(out, len, "%02d:%02d", when_tm.tm_hour, when_tm.tm_min);
  } else {
    snprintf(out, len, "%d월 %d일 %02d:%02d", when_tm.tm_mon + 1, when_tm.tm_mday, when_tm.tm_hour, when_tm.tm_min);
  }
}

// 공유 저장소의 내용을 화면 페이지 형식으로 바꾼다 (PC 도우미의 collectors/fmt.py 와 같은 규칙)
//   {"claude_code": {"name": "Claude Code", "as_of": 시각, "windows": [[이름, 사용률, 초기화 시각], ...]}, ...}
// 저장소에 적힌 구간 이름(여러 PC 가 같은 이름으로 맞춰 쓴다)을 화면에 보일 이름으로 바꾼다
static const char *window_label(const char *key) {
  if (strcmp(key, "5h") == 0) return "5시간";
  if (strcmp(key, "Week") == 0) return "주간";
  return key;
}

static void build_pages(JsonObjectConst shared, JsonArray services) {
  time_t now = time(nullptr);
  char text[32];
  for (JsonPairConst item : shared) {
    JsonObjectConst snapshot = item.value();
    JsonObject page = services.add<JsonObject>();
    page["name"] = snapshot["name"] | item.key().c_str();
    format_when((time_t)(snapshot["as_of"] | 0.0), text, sizeof(text));
    page["note"] = text;

    // 5시간, 주간은 값이 없어도(초기화된 뒤 아직 안 썼으면 서버가 알려주지 않는다) 0% 로 보여준다
    JsonArray bars = page["bars"].to<JsonArray>();
    static const char *const ALWAYS[] = {"5h", "Week"};
    for (const char *key : ALWAYS) {
      JsonArrayConst window;
      for (JsonArrayConst w : snapshot["windows"].as<JsonArrayConst>()) {
        if (strcmp(w[0] | "", key) == 0) window = w;
      }
      time_t resets_at = window.isNull() ? 0 : (time_t)(window[2] | 0.0);
      bool reset = resets_at != 0 && resets_at <= now;
      int pct = reset ? 0 : constrain((int)lround(window[1] | 0.0), 0, 100);
      JsonObject bar = bars.add<JsonObject>();
      bar["label"] = window_label(key);
      bar["pct"] = pct;
      snprintf(text, sizeof(text), "%d%%", pct);
      bar["text"] = text;
      text[0] = 0;
      if (!reset && resets_at != 0) format_when(resets_at, text, sizeof(text));
      bar["note"] = text;
      if (!reset && resets_at != 0) bar["resets"] = (uint32_t)resets_at;
    }
  }
}

#define RETRY_MS 20000
static bool fetch(const char *from);

static bool poll(const char *from) {
  // 암호화 통신의 나머지(드라이버 등)가 내부 RAM 을 조금 쓰므로, 카메라용으로 잡아 둔 자리를 잠깐 빌린다
  camera_lend_reserve(true);
  bool ok = fetch(from);
  camera_lend_reserve(false);
  return ok;
}

static bool fetch(const char *from) {
  NetworkClientSecure client;
  // 화면에 보여줄 사용률 숫자만 읽으므로 서버 인증서 검증은 생략한다
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  if (!http.begin(client, from)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[cloud] fetch failed: %d (internal free %u, largest %u)\n", code,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument shared(PSRAM_JSON);
  if (deserializeJson(shared, body) || !shared.is<JsonObjectConst>()) {
    Serial.println("[cloud] bad data");
    return false;
  }
  JsonDocument message(PSRAM_JSON);
  build_pages(shared.as<JsonObjectConst>(), message["services"].to<JsonArray>());
  String out;
  serializeJson(message, out);
  link_submit(LINK_CLOUD, out.c_str(), out.length());
  return true;
}

// 암호화 통신(mbedTLS)이 쓰는 메모리(연결 하나에 40KB 안팎)를 큰 메모리(PSRAM)에서 받게 한다.
// 기본 설정은 내부 RAM 에서만 받아, 블루투스·Wi-Fi 와 겹치면 내부 RAM 이 바닥났다.
static void *tls_calloc(size_t n, size_t size) {
  void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void cloud_task(void *) {
  uint32_t last_ms = 0;
  bool polled = false;
  char from[URL_MAX];

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(from, url, sizeof(from));
    xSemaphoreGive(lock);

    // PC 가 연결되어 있으면 PC 가 보내주는 값을 쓴다
    Link active = link_active();
    if (active != LINK_NONE && active != LINK_CLOUD) {
      polled = false;
      continue;
    }
    if (!settings_link_allowed(LINK_CLOUD) || !from[0] || WiFi.status() != WL_CONNECTED || !clock_ready()) continue;
    if (polled && millis() - last_ms < POLL_MS) continue;
    polled = true;
    last_ms = millis();
    // 실패하면(블루투스가 막 끊겨 메모리가 아직 정리 중일 때 등) 20초 뒤에 다시 해 본다
    if (!poll(from)) last_ms = millis() - POLL_MS + RETRY_MS;
  }
}

void cloud_init() {
  lock = xSemaphoreCreateMutex();
  prefs.begin("llm-deck");
  prefs.getString("sync_url", url, sizeof(url));
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
  mbedtls_platform_set_calloc_free(tls_calloc, heap_caps_free);
  // 암호화 통신(TLS)이 스택을 많이 써서 넉넉히 주되, 큰 메모리(PSRAM)에 둔다 (이 작업은 플래시에 쓰지 않는다)
  xTaskCreatePinnedToCoreWithCaps(cloud_task, "cloud", 12288, NULL, 1, NULL, 1, MALLOC_CAP_SPIRAM);
}

void cloud_set_url(const char *value) {
  if (strlen(value) >= URL_MAX || strcmp(value, url) == 0) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(url, value, sizeof(url));
  xSemaphoreGive(lock);
  prefs.putString("sync_url", url);
  Serial.println("[cloud] shared address saved");
}
