#include <cassert>
#include <cstddef>
#include <cstring>

#include "helper_pkg_name.h"

int main() {
  assert(helper_pkg_name_ok("llm-deck-companion-windows.zip"));
  assert(helper_pkg_name_ok("version.txt"));
  assert(!helper_pkg_name_ok(""));
  assert(!helper_pkg_name_ok(nullptr));
  assert(!helper_pkg_name_ok("../x"));
  assert(!helper_pkg_name_ok(".hidden"));
  assert(!helper_pkg_name_ok("a/b.zip"));
  assert(!helper_pkg_name_ok("한글.zip"));
  assert(!helper_pkg_name_ok("x123456789012345678901234567890123456789012"));
}
