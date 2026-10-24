#include "message_queue.h"
#include "bounded_buffer.h"
#include <assert.h>
#include <string>

static bool fail_alloc = false;
static void *allocate(size_t n) { return fail_alloc ? nullptr : malloc(n); }
static std::string pop(MessageQueue &q, int from = 1) {
  MessageQueue::Item item;
  assert(q.pop(item));
  assert(item.from == from);
  std::string data(item.data, item.len);
  free(item.data);
  return data;
}

int main() {
  {
    MessageQueue q;
    // 상태 폭주가 이미 받은 촬영 명령을 밀어내지 않는다.
    assert(q.push(1, true, 0, "shoot", 5));
    for (int i = 0; i < 100; ++i) assert(q.push(1, true, 1, "state", 5));
    assert(pop(q) == "shoot");
    assert(pop(q) == "state");
    MessageQueue::Item item;
    assert(!q.pop(item));
  }
  {
    MessageQueue q;
    for (int i = 0; i < 8; ++i) {
      std::string cmd = std::to_string(i);
      assert(q.push(1, true, 0, cmd.data(), cmd.size()));
    }
    assert(!q.push(1, true, 0, "overflow", 8));
    for (int i = 0; i < 8; ++i) assert(pop(q) == std::to_string(i));
  }
  {
    MessageQueue q(allocate);
    assert(q.push(1, true, 1, "old", 3));
    fail_alloc = true;
    assert(!q.push(1, true, 1, "new", 3));
    fail_alloc = false;
    assert(pop(q) == "old");
  }
  {
    MessageQueue q;
    assert(q.push(1, true, 1, "old", 3));
    assert(q.push(1, true, 0, "command", 7));
    assert(q.push(2, true, 1, "other-pc", 8));
    assert(q.push(1, true, 2, "other-fields", 12));
    assert(q.push(1, true, 1, "new", 3));
    assert(pop(q) == "old");
    assert(pop(q) == "command");
    assert(pop(q, 2) == "other-pc");
    assert(pop(q) == "other-fields");
    assert(pop(q) == "new");
  }
  {
    MessageQueue q;
    std::string large(MessageQueue::MAX_MESSAGE, 'x');
    assert(q.push(1, true, 1, large.data(), large.size()));
    assert(!q.push(2, true, 1, large.data(), large.size())); // 상태용 바이트 상한
    assert(q.push(1, true, 0, large.data(), large.size())); // 명령용 여유는 따로 확보
    large += 'x';
    assert(!q.push(1, true, 0, large.data(), large.size()));
  }
  {
    uint8_t bytes[4] = {};
    BoundedBuffer b(bytes, sizeof(bytes));
    assert(b.append(reinterpret_cast<const uint8_t *>("ab"), 2) == 2);
    assert(b.append(reinterpret_cast<const uint8_t *>("cd"), 2) == 2);
    assert(b.append(reinterpret_cast<const uint8_t *>("e"), 1) == 0);
    assert(b.failed() && b.size() == 4 && memcmp(bytes, "abcd", 4) == 0);
    assert(b.append(reinterpret_cast<const uint8_t *>("f"), 1) == 0);
  }
}
