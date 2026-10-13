#include "cloud.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

#include "link.h"

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
  strftime(out, len, today ? "%H:%M" : "%b %d %H:%M", &when_tm);
}

// 공유 저장소의 내용을 화면 페이지 형식으로 바꾼다 (PC 도우미의 collectors/fmt.py 와 같은 규칙)
//   {"claude_code": {"name": "Claude Code", "as_of": 시각, "windows": [[이름, 사용률, 초기화 시각], ...]}, ...}
static void build_pages(JsonObjectConst shared, JsonArray services) {
  time_t now = time(nullptr);
  char text[24];
  for (JsonPairConst item : shared) {
    JsonObjectConst snapshot = item.value();
    JsonObject page = services.add<JsonObject>();
    page["name"] = snapshot["name"] | item.key().c_str();
    format_when((time_t)(snapshot["as_of"] | 0.0), text, sizeof(text));
    page["note"] = text;

    JsonArray bars = page["bars"].to<JsonArray>();
    for (JsonArrayConst window : snapshot["windows"].as<JsonArrayConst>()) {
      time_t resets_at = (time_t)(window[2] | 0.0);
      bool reset = resets_at != 0 && resets_at <= now;
      int pct = reset ? 0 : constrain((int)lround(window[1] | 0.0), 0, 100);
      JsonObject bar = bars.add<JsonObject>();
      bar["label"] = window[0] | "";
      bar["pct"] = pct;
      snprintf(text, sizeof(text), "%d%%", pct);
      bar["text"] = text;
      text[0] = 0;
      if (!reset && resets_at != 0) format_when(resets_at, text, sizeof(text));
      bar["note"] = text;
    }
  }
}

static void poll(const char *from) {
  NetworkClientSecure client;
  // 화면에 보여줄 사용률 숫자만 읽으므로 서버 인증서 검증은 생략한다
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  if (!http.begin(client, from)) return;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[cloud] fetch failed: %d\n", code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();

  JsonDocument shared;
  if (deserializeJson(shared, body) || !shared.is<JsonObjectConst>()) {
    Serial.println("[cloud] bad data");
    return;
  }
  JsonDocument message;
  build_pages(shared.as<JsonObjectConst>(), message["services"].to<JsonArray>());
  String out;
  serializeJson(message, out);
  link_submit(LINK_CLOUD, out.c_str(), out.length());
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
    if (!from[0] || WiFi.status() != WL_CONNECTED || !clock_ready()) continue;
    if (polled && millis() - last_ms < POLL_MS) continue;
    polled = true;
    last_ms = millis();
    poll(from);
  }
}

void cloud_init() {
  lock = xSemaphoreCreateMutex();
  prefs.begin("llm-deck");
  prefs.getString("sync_url", url, sizeof(url));
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
  // 암호화 통신(TLS)이 메모리를 많이 써서 스택을 넉넉히 준다
  xTaskCreatePinnedToCore(cloud_task, "cloud", 12288, NULL, 1, NULL, 1);
}

void cloud_set_url(const char *value) {
  if (strlen(value) >= URL_MAX || strcmp(value, url) == 0) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(url, value, sizeof(url));
  xSemaphoreGive(lock);
  prefs.putString("sync_url", url);
  Serial.println("[cloud] shared address saved");
}
