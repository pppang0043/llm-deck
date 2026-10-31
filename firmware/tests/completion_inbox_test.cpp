#include "completion_inbox.h"
#include <cassert>
#include <cstring>
#include <string>
int main() {
  CompletionInbox inbox;
  char snapshot[CompletionInbox::SNAPSHOT_SIZE];
  inbox.push("ORCA 작업 완료\n첫 번째");
  inbox.push("ORCA 작업 완료\n두 번째");
  assert(inbox.count() == 2);  // 배너 시간 만료는 큐를 바꾸지 않는다
  inbox.take(snapshot, sizeof(snapshot));
  assert(std::strstr(snapshot, "두 번째") < std::strstr(snapshot, "첫 번째"));
  assert(inbox.count() == 0);
  inbox.take(snapshot, sizeof(snapshot));  // 다른 화면/다음 열람은 이미 읽은 알림을 되살리지 않는다
  assert(snapshot[0] == 0);
  for (int i = 0; i < 12; ++i) { char n[8]; std::snprintf(n, sizeof(n), "task-%d", i); inbox.push(n); }
  assert(inbox.count() == 8);
  inbox.take(snapshot, sizeof(snapshot));
  assert(std::strstr(snapshot, "task-11") && std::strstr(snapshot, "task-4"));
  assert(!std::strstr(snapshot, "task-3"));
  std::string korean;
  for (int i = 0; i < 100; ++i) korean += "완";
  inbox.push(korean.c_str());
  assert(std::strlen(inbox.latest()) < CompletionInbox::TEXT_SIZE);
  assert(std::strlen(inbox.latest()) % 3 == 0);
  inbox.take(snapshot, sizeof(snapshot));
  inbox.push("new completion while shade is open");
  inbox.take(snapshot, sizeof(snapshot));
  assert(!std::strcmp(snapshot, "new completion while shade is open"));
  assert(inbox.count() == 0);
}
