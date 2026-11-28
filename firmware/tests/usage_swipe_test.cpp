#include <cassert>
#include "usage_swipe.h"

int main() {
  assert(usage_swipe_column(true, false, true, 0) == 1);
  assert(usage_swipe_column(false, true, true, 1) == 0);
  assert(usage_swipe_column(true, false, true, 1) == 0);
  assert(usage_swipe_column(false, true, true, 0) == -1);
  assert(usage_swipe_column(false, false, true, 0) == -1);
  assert(usage_swipe_column(false, false, true, 1) == -1);
  assert(usage_swipe_column(true, false, false, 0) == -1);
  assert(usage_swipe_column(true, false, false, 1) == -1);
}
