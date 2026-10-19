#include "alerts.h"

#include <map>
#include <string>

#include "history.h"
#include "link.h"
#include "settings.h"
#include "speaker.h"
#include "ui.h"

// 서비스 이름 + 구간 이름 -> 지난번 사용률
static std::map<std::string, int> last_pct;
// 서비스 이름 + 구간 이름 -> 소진 미리 알림 상태
static std::map<std::string, PreAlert> pre;

// 남은 초 -> "40분", "3시간 20분", "9시간"
static void fmt_left(long s, char *out, size_t size) {
  long min = (s + 59) / 60;
  if (min < 60) {
    snprintf(out, size, "%ld분", min < 1 ? 1 : min);
  } else if (min % 60 == 0) {
    snprintf(out, size, "%ld시간", min / 60);
  } else {
    snprintf(out, size, "%ld시간 %ld분", min / 60, min % 60);
  }
}

void alerts_check(JsonArrayConst services) {
  char text[128] = "";
  const char *sound = nullptr;
  time_t now = time(nullptr);
  for (JsonObjectConst svc : services) {
    const char *name = svc["name"] | "";
    for (JsonObjectConst bar : svc["bars"].as<JsonArrayConst>()) {
      const char *label = bar["label"] | "";
      int pct = bar["pct"] | -1;
      if (pct < 0) continue;
      std::string key = std::string(name) + "/" + label;
      auto it = last_pct.find(key);
      int before = it == last_pct.end() ? -1 : it->second;
      last_pct[key] = pct;
      if (before < 0 || !settings_limit_alerts()) continue;

      if (before < 90 && pct >= 90) {
        snprintf(text, sizeof(text), "%s %s\n%d%% 사용", name, label, pct);
        sound = "limit_warn";
      } else if (before < 80 && pct >= 80 && !sound) {
        snprintf(text, sizeof(text), "%s %s\n%d%% 사용", name, label, pct);
        sound = "limit_warn";
      } else if (before >= 20 && pct <= before - 20 && pct < 10 && !sound) {
        // 크게 쓰던 구간이 거의 0 으로 돌아왔다
        snprintf(text, sizeof(text), "%s %s\n한도가 초기화되었습니다", name, label);
        sound = "limit_reset";
      }

      // 소진 미리 알림: "16:40 소진" 과 같은 예상(history_forecast). 이번 칸 기록이 없는 오래된 값으로는 예상하지 않는다.
      uint32_t resets = bar["resets"] | 0u;
      time_t full = history_forecast(name, label, pct, resets);
      long left = full ? (long)(full - now) : -1;
      long limit = strcmp(label, "5시간") == 0 ? 3600L : 12 * 3600L;
      bool dropped = pct <= before - 20;
      if (prealert_step(pre[key], now, resets, dropped, left, limit, sound != nullptr)) {
        char when[24];
        fmt_left(left, when, sizeof(when));
        snprintf(text, sizeof(text), "%s %s 한도\n이 속도면 %s 뒤 소진", name, label, when);
        sound = "limit_warn";
      }
    }
  }
  if (!sound) return;
  Serial.printf("[alert] %s\n", sound);
  ui_toast(text, 6000);
  if (speaker_play(sound)) return;
  char event[48];
  snprintf(event, sizeof(event), "{\"sound\":\"%s\"}", sound);
  link_send_event(event);
}
