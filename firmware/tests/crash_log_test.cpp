#include "crash_log.h"
#include <assert.h>
#include <string.h>
#include <string>

static const char BUILD[] = "4cf9c2440011223344ff";

static std::string take(CrashLog &log, const char *reason = "panic") {
  char json[128];
  return log.take(json, sizeof(json), reason, BUILD) ? json : "";
}

int main() {
  {
    // 처음 켜면 RTC 값이 흐트러져 있다: 마법 값이 다르면 쓰레기를 믿지 않는다
    CrashLog log;
    memset(&log, 0xA5, sizeof(log));
    log.boot(false, false, 0, 0);
    assert(log.count == 0 && take(log).empty());
  }
  {
    // 도우미와 맞춘 형식 그대로 나온다. 한 번만 알린다.
    CrashLog log;
    memset(&log, 0, sizeof(log));
    log.boot(false, true, 4, 57);
    assert(take(log) == "{\"crash\":{\"reason\":\"panic\",\"count\":1,\"uptime_s\":57,\"build\":\"4cf9c244\"}}");
    assert(take(log).empty());
  }
  {
    // 연달아 죽으면 쌓이고, 알리지 못한 동안에도 이어서 센다
    CrashLog log;
    memset(&log, 0, sizeof(log));
    log.boot(false, true, 4, 60);
    log.boot(false, true, 7, 61);
    log.boot(false, true, 4, 62);
    assert(log.count == 3 && take(log).find("\"count\":3,\"uptime_s\":62") != std::string::npos);
    // 알린 뒤에도 5분을 못 넘기고 또 죽으면 횟수는 이어진다
    log.boot(false, true, 4, 30);
    assert(take(log).find("\"count\":4") != std::string::npos);
  }
  {
    // 정상 재시작(업데이트 등)은 횟수를 지우지 않고, 못 알린 기록도 남긴다
    CrashLog log;
    memset(&log, 0, sizeof(log));
    log.boot(false, true, 4, 10);
    log.boot(false, false, 3, 20);
    assert(log.count == 1 && take(log).find("\"count\":1,\"uptime_s\":10") != std::string::npos);
  }
  {
    // 5분 살아남으면 연속 횟수는 지워지지만 아직 못 알린 것은 그대로 알린다
    CrashLog log;
    memset(&log, 0, sizeof(log));
    log.boot(false, true, 4, 10);
    log.boot(false, true, 4, 11);
    log.survived();
    assert(log.count == 0);
    assert(take(log).find("\"count\":2") != std::string::npos);
    log.boot(false, true, 4, 12);
    assert(take(log).find("\"count\":1") != std::string::npos);
  }
  {
    // 전원을 새로 넣으면 앞의 기록은 없던 것으로 한다
    CrashLog log;
    memset(&log, 0, sizeof(log));
    log.boot(false, true, 4, 10);
    log.boot(true, false, 1, 0);
    assert(log.count == 0 && take(log).empty());
  }
  return 0;
}
