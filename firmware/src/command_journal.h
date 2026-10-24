#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// 호출자가 잠근다. 실행 전에 RUNNING 을 원자적으로 저장하고, 루틴이 돌아온 뒤
// DONE/FAILED 를 저장한다. RUNNING 으로 부팅하면 재실행하지 않고 uncertain 을 돌려준다.
// 오래된 client 를 자동으로 지우지 않는다: 지우면 그 PC 의 옛 명령이 다시 실행될 수 있다.
class CommandJournal {
 public:
  static constexpr unsigned CLIENTS = 8, QUEUED = 8;
  enum State : uint32_t { EMPTY, RUNNING, DONE, FAILED };
  struct Request {
    char client[33] = {};
    uint32_t seq = 0;
    uint8_t digest[32] = {};
  };
  struct Record { Request request; State state = EMPTY; };
  struct Storage { uint32_t magic = 0x434d4431; Record records[CLIENTS]; };
  using Save = bool (*)(const Storage &);
  explicit CommandJournal(Save save) : save_(save) {}

  static bool valid_client(const char *s) {
    if (!s || strlen(s) != 32) return false;
    for (unsigned i = 0; i < 32; ++i) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
  }
  bool load(const void *data, size_t len) {
    if (!data && !len) { storage_ = Storage{}; ready_ = true; return true; }
    ready_ = false;
    if (!data || len != sizeof(Storage)) return false;
    Storage loaded;
    memcpy(&loaded, data, len);
    if (loaded.magic != Storage{}.magic) return false;
    for (const auto &r : loaded.records) {
      if (r.state > FAILED || (r.state != EMPTY && (r.request.client[32] != 0 ||
          !valid_client(r.request.client) || !r.request.seq))) return false;
    }
    storage_ = loaded;
    ready_ = true;
    return true;
  }
  const char *status(const char *client, uint32_t seq) const {
    if (!ready_) return "storage_error";
    for (unsigned i = 0; i < QUEUED; ++i)
      if (queued_[i].seq == seq && strcmp(queued_[i].client, client) == 0) return "queued";
    int slot = find(client);
    if (slot < 0 || seq > storage_.records[slot].request.seq) return "unknown";
    if (seq < storage_.records[slot].request.seq) return "stale";
    switch (storage_.records[slot].state) {
      case DONE: return "done";
      case FAILED: return "failed";
      default: return live_ == slot ? "running" : "uncertain";
    }
  }
  // "new" 일 때만 ticket 을 큐에 넣는다. 나머지는 이전 결과 또는 거절이다.
  const char *reserve(const Request &req, int &ticket) {
    ticket = -1;
    if (!ready_) return "storage_error";
    if (!valid_client(req.client) || !req.seq) return "invalid";
    for (unsigned i = 0; i < QUEUED; ++i) {
      if (queued_[i].seq && strcmp(queued_[i].client, req.client) == 0) {
        if (queued_[i].seq != req.seq) return "busy";
        return memcmp(queued_[i].digest, req.digest, 32) ? "conflict" : "queued";
      }
    }
    int slot = find(req.client);
    if (slot >= 0 && req.seq <= storage_.records[slot].request.seq) {
      const Record &r = storage_.records[slot];
      if (req.seq == r.request.seq && memcmp(req.digest, r.request.digest, 32)) return "conflict";
      return status(req.client, req.seq);
    }
    // 아직 저장하지 않은 다른 client 의 예약 자리도 센다.
    unsigned used = 0;
    for (const auto &r : storage_.records) if (r.state != EMPTY) ++used;
    for (const auto &q : queued_) if (q.seq && find(q.client) < 0) ++used;
    if (slot < 0 && used >= CLIENTS) return "clients_full";
    for (unsigned i = 0; i < QUEUED; ++i) if (!queued_[i].seq) {
      queued_[i] = req;
      ticket = i;
      return "new";
    }
    return "busy";
  }
  void cancel(int ticket) { if (ticket >= 0 && ticket < (int)QUEUED) queued_[ticket] = Request{}; }
  bool start(int ticket, Request &req) {
    if (!ready_ || ticket < 0 || ticket >= (int)QUEUED || !queued_[ticket].seq) return false;
    req = queued_[ticket];
    cancel(ticket);
    int slot = find(req.client);
    if (slot < 0) for (unsigned i = 0; i < CLIENTS; ++i) if (storage_.records[i].state == EMPTY) { slot = i; break; }
    if (slot < 0) return false;
    Storage next = storage_;
    next.records[slot] = {req, RUNNING};
    if (!save_(next)) { ready_ = false; return false; } // 저장하지 못하면 실행하지 않는다
    storage_ = next;
    live_ = slot;
    return true;
  }
  bool finish(const Request &req, bool ok) {
    int slot = find(req.client);
    if (slot < 0 || storage_.records[slot].request.seq != req.seq) return false;
    Storage next = storage_;
    next.records[slot].state = ok ? DONE : FAILED;
    live_ = -1;
    if (!save_(next)) return false; // RUNNING 을 남긴다. 다시 받아도 실행하지 않는다
    storage_ = next;
    return true;
  }
 private:
  int find(const char *client) const {
    for (unsigned i = 0; i < CLIENTS; ++i)
      if (storage_.records[i].state != EMPTY && strcmp(storage_.records[i].request.client, client) == 0) return i;
    return -1;
  }
  Storage storage_;
  Request queued_[QUEUED];
  Save save_;
  bool ready_ = true;
  int live_ = -1;
};
