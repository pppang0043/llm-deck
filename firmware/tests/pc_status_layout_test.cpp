#include "pc_status_layout.h"
#include <cassert>

int main() {
  // 원래 화면(마이크 버튼 제외)과 미러링 화면, 두 방향 모두 검사한다.
  const int sizes[][2] = {{268, 208}, {240, 244}, {320, 208}, {240, 288}};
  for (const auto &size : sizes) {
    const auto l = PcStatusLayout::fit(size[0], size[1]);
    assert(3 * l.card_width + 12 <= l.width);
    assert(l.card_width >= 70);
    assert(l.card_y + l.card_height <= l.disk_heading_y);
    assert(l.disk_heading_y + 24 <= l.disk_y);
    assert(l.disk_slots >= 2);
    assert(l.disk_y + (l.disk_slots - 1) * l.disk_stride + l.disk_height + 6 <= l.footer_y);
    assert(l.footer_y + 32 <= l.height);
    // 26개 드라이브가 있어도 한 페이지의 항목은 같은 고정 영역 안에 있다.
    for (int count = 0; count <= 26; ++count) {
      int visible = count < l.disk_slots ? count : l.disk_slots;
      if (visible) assert(l.disk_y + (visible - 1) * l.disk_stride + l.disk_height <= l.footer_y - 6);
    }
  }
}
