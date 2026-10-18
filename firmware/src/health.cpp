#include "health.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <time.h>

// 재시작(패닉, 감시 장치, 소프트웨어 재시작)해도 지워지지 않는 곳. 전원을 새로 넣으면 지워진다.
#define RTC_MAGIC 0x4845414C
RTC_NOINIT_ATTR static uint32_t rtc_magic;
RTC_NOINIT_ATTR static uint32_t rtc_uptime_s;
RTC_NOINIT_ATTR static uint32_t rtc_min_internal;

// loop() 한 바퀴의 한계시간. 평소에는 10ms, 화면을 새로 그릴 때도 0.2초 안쪽이다.
#define LOOP_LIMIT_MS 20000

static Preferences prefs;
static bool watching = false;
static esp_reset_reason_t reason;
static bool error_reset = false;
static bool when_saved = false;
// 저장된 마지막 오류
static uint8_t err_reason = 0;
static uint32_t err_uptime_s = 0, err_min_internal = 0, err_count = 0;
static int64_t err_when = 0;

static const char *reason_text(int r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "전원 켬";
    case ESP_RST_SW:
      return "재시작";
    case ESP_RST_USB:
    case ESP_RST_EXT:
    case ESP_RST_JTAG:
      return "업로드/리셋 버튼";
    case ESP_RST_DEEPSLEEP:
      return "잠에서 깸";
    case ESP_RST_PANIC:
      return "프로그램 멈춤";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return "응답 없음 (감시 장치)";
    case ESP_RST_BROWNOUT:
      return "전압 부족";
    default:
      return "알 수 없음";
  }
}

static bool is_error(int r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
         r == ESP_RST_BROWNOUT;
}

static void format_duration(uint32_t s, char *out, size_t size) {
  if (s >= 3600) {
    snprintf(out, size, "%lu시간 %lu분", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
  } else if (s >= 60) {
    snprintf(out, size, "%lu분", (unsigned long)(s / 60));
  } else {
    snprintf(out, size, "%lu초", (unsigned long)s);
  }
}

void health_init() {
  reason = esp_reset_reason();
  bool have_rtc = rtc_magic == RTC_MAGIC;
  uint32_t prev_uptime = have_rtc ? rtc_uptime_s : 0;
  uint32_t prev_min = have_rtc ? rtc_min_internal : 0;

  prefs.begin("health");
  err_count = prefs.getUInt("count", 0);
  err_reason = prefs.getUChar("reason", 0);
  err_uptime_s = prefs.getUInt("uptime", 0);
  err_min_internal = prefs.getUInt("min", 0);
  err_when = prefs.getLong64("when", 0);

  error_reset = is_error(reason);
  if (error_reset) {
    err_count++;
    err_reason = reason;
    err_uptime_s = prev_uptime;
    err_min_internal = prev_min;
    err_when = 0;  // 시각은 시계가 맞춰진 뒤에 적는다
    prefs.putUInt("count", err_count);
    prefs.putUChar("reason", err_reason);
    prefs.putUInt("uptime", err_uptime_s);
    prefs.putUInt("min", err_min_internal);
    prefs.putLong64("when", 0);
  }
  char up[24];
  format_duration(prev_uptime, up, sizeof(up));
  Serial.printf("[health] reset: %s (%d)%s%s, internal min before %u, errors so far %lu\n", reason_text(reason),
                (int)reason, have_rtc ? ", ran " : "", have_rtc ? up : "", (unsigned)prev_min,
                (unsigned long)err_count);

  rtc_magic = RTC_MAGIC;
  rtc_uptime_s = 0;
  rtc_min_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

void health_loop() {
  static uint32_t checked_ms = 0;
  if (millis() - checked_ms < 1000) return;
  checked_ms = millis();
  rtc_uptime_s = millis() / 1000;
  rtc_min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  // 오류로 켜졌으면, 시계가 맞춰진 뒤 그 시각(=이번에 켜진 시각)을 적어 둔다
  if (error_reset && !when_saved && time(nullptr) > 1700000000) {
    when_saved = true;
    err_when = (int64_t)time(nullptr) - millis() / 1000;
    prefs.putLong64("when", err_when);
  }
}

const char *health_boot_reason() {
  return reason_text(reason);
}

bool health_last_error(char *out, size_t size) {
  if (err_count == 0) return false;
  char up[24];
  format_duration(err_uptime_s, up, sizeof(up));
  char when[24] = "";
  if (err_when > 0) {
    time_t t = (time_t)err_when;
    struct tm local;
    localtime_r(&t, &local);
    snprintf(when, sizeof(when), "%d/%d %02d:%02d ", local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
  }
  snprintf(out, size, "%s%s (%s 켜진 뒤)", when, reason_text(err_reason), up);
  return true;
}

uint32_t health_error_count() {
  return err_count;
}

void health_clear() {
  err_count = 0;
  err_reason = 0;
  err_uptime_s = err_min_internal = 0;
  err_when = 0;
  error_reset = false;
  prefs.clear();
  Serial.println("[health] error record cleared");
}

void health_watch_start() {
  // 기본 감시 장치는 CPU0 의 쉬는 작업만 본다(5초). loop() 가 도는 작업도 더해 한계시간을 같이 맞춘다.
  esp_task_wdt_config_t config = {
      .timeout_ms = LOOP_LIMIT_MS,
      .idle_core_mask = 1 << 0,
      .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&config);
  watching = esp_task_wdt_add(NULL) == ESP_OK;
  Serial.printf("[health] loop watchdog %s (%us)\n", watching ? "on" : "failed", LOOP_LIMIT_MS / 1000);
}

void health_watch_feed() {
  if (watching) esp_task_wdt_reset();
}

void health_watch_pause(bool paused) {
  if (!watching) return;
  if (paused) {
    esp_task_wdt_delete(NULL);
  } else {
    esp_task_wdt_add(NULL);
  }
}
