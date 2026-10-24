#include "command_journal.h"
#include "command_protocol.h"
#include <assert.h>
#include <stdio.h>

static CommandJournal::Storage disk;
static bool fail_save = false;
static bool save(const CommandJournal::Storage &s) { if (fail_save) return false; disk = s; return true; }
static CommandJournal::Request request(unsigned client, uint32_t seq) {
  CommandJournal::Request r;
  snprintf(r.client, sizeof(r.client), "%032x", client);
  r.seq = seq;
  r.digest[0] = 42;
  return r;
}
static void equal(const char *a, const char *b) { assert(strcmp(a, b) == 0); }

int main() {
  int ticket;
  auto r = request(1, 1);
  CommandJournal::Request executed;
  {
    CommandJournal j(save);
    equal(j.reserve(r, ticket), "new");
    int first = ticket;
    equal(j.status(r.client, 1), "queued");
    equal(j.reserve(r, ticket), "queued");
    auto changed = r; changed.digest[1] = 1;
    equal(j.reserve(changed, ticket), "conflict");
    equal(j.reserve(request(1, 2), ticket), "busy");
    assert(j.start(first, executed));
    equal(j.status(r.client, 1), "running");
    // 부작용 직전/직후 어느 쪽에서 전원이 끊겨도 같은 명령은 다시 실행하지 않는다.
    CommandJournal reboot(save);
    assert(reboot.load(&disk, sizeof(disk)));
    equal(reboot.reserve(r, ticket), "uncertain");
    assert(ticket == -1);
    assert(j.finish(executed, true));
    CommandJournal reboot_done(save);
    assert(reboot_done.load(&disk, sizeof(disk)));
    equal(reboot_done.reserve(r, ticket), "done");
    equal(reboot_done.reserve(changed, ticket), "conflict");
    equal(reboot_done.reserve(request(1, 2), ticket), "new");
    assert(reboot_done.start(ticket, executed));
    assert(reboot_done.finish(executed, false));
    equal(reboot_done.status(r.client, 2), "failed");
    equal(reboot_done.reserve(r, ticket), "stale");
  }
  {
    CommandJournal j(save);
    equal(j.reserve(r, ticket), "new");
    fail_save = true;
    assert(!j.start(ticket, executed)); // 실행 전 영구 기록 실패: 실행 금지
    equal(j.status(r.client, 1), "storage_error");
    fail_save = false;
  }
  {
    CommandJournal j(save);
    equal(j.reserve(r, ticket), "new");
    assert(j.start(ticket, executed));
    fail_save = true;
    assert(!j.finish(executed, true));
    equal(j.status(r.client, 1), "uncertain");
    fail_save = false;
    CommandJournal reboot(save);
    assert(reboot.load(&disk, sizeof(disk)));
    equal(reboot.reserve(r, ticket), "uncertain");
  }
  {
    CommandJournal j(save);
    for (unsigned i = 1; i <= CommandJournal::CLIENTS; ++i) {
      equal(j.reserve(request(i, 1), ticket), "new");
      assert(j.start(ticket, executed));
      assert(j.finish(executed, true));
    }
    equal(j.reserve(request(9, 1), ticket), "clients_full");
    equal(j.reserve(r, ticket), "done"); // 오래된 client 를 밀어내지 않는다
    auto corrupt = disk; corrupt.magic = 0;
    assert(!j.load(&corrupt, sizeof(corrupt)));
    equal(j.reserve(r, ticket), "storage_error");
  }
  {
    CommandJournal j(save);
    equal(j.reserve(r, ticket), "new");
    j.cancel(ticket); // 수신 큐 포화는 실행 기록을 만들지 않는다
    equal(j.reserve(r, ticket), "new");
  }
  {
    JsonDocument doc;
    assert(!deserializeJson(doc, "{\"camera\":\"record\"}"));
    assert(valid_command(doc.as<JsonObjectConst>()));
    assert(!deserializeJson(doc, "{\"camera\":\"delete_all\"}"));
    assert(!valid_command(doc.as<JsonObjectConst>()));
    assert(!deserializeJson(doc, "{\"camera\":\"shoot\",\"unknown\":true}"));
    assert(!valid_command(doc.as<JsonObjectConst>()));
    assert(!deserializeJson(doc, "{\"links\":{\"usb\":false}}"));
    assert(valid_command(doc.as<JsonObjectConst>()));
    assert(!deserializeJson(doc, "{\"links\":{\"usb\":\"false\"}}"));
    assert(!valid_command(doc.as<JsonObjectConst>()));
  }
}
