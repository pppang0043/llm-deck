#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// 이상 재시작 기록. RTC_NOINIT 에 두어 재시작해도 남는다 (전원을 새로 넣으면 지운다).
// count 는 연달아 일어난 횟수로, 오래 살아남은 부팅(survived)이 지운다. pending 은 PC 에 아직 알리지 않은 횟수다.
// 부팅마다 죽어 PC 에 못 알렸거나, 알리기 전에 소프트웨어 재시작을 해도 다음 부팅이 이어서 알린다.
struct CrashLog {
  static constexpr uint32_t MAGIC = 0x43525348;
  uint32_t magic, count, pending, reason, uptime_s;

  void boot(bool power_on, bool crashed, uint32_t why, uint32_t ran_s) {
    if (magic != MAGIC || power_on) {
      magic = MAGIC;
      count = pending = reason = uptime_s = 0;
    }
    if (!crashed) return;
    count++;
    pending = count;
    reason = why;
    uptime_s = ran_s;
  }

  void survived() { count = 0; }

  // 알릴 것이 있으면 JSON 을 만들고 true (한 번만). build 는 앞 8자만 쓴다
  bool take(char *out, size_t size, const char *reason_name, const char *build) {
    if (!pending) return false;
    snprintf(out, size, "{\"crash\":{\"reason\":\"%s\",\"count\":%lu,\"uptime_s\":%lu,\"build\":\"%.8s\"}}", reason_name,
             (unsigned long)pending, (unsigned long)uptime_s, build);
    pending = 0;
    return true;
  }
};
