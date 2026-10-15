#include "alerts.h"

#include <map>
#include <string>

#include "link.h"
#include "settings.h"
#include "ui.h"

// 서비스 이름 + 구간 이름 -> 지난번 사용률
static std::map<std::string, int> last_pct;

void alerts_check(JsonArrayConst services) {
  char text[96] = "";
  const char *sound = nullptr;
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
    }
  }
  if (!sound) return;
  Serial.printf("[alert] %s\n", sound);
  ui_toast(text, 6000);
  char event[48];
  snprintf(event, sizeof(event), "{\"sound\":\"%s\"}", sound);
  link_send_event(event);
}
