#include "pc_status_layout.h"
#include <cassert>

int main() {
  // 원래 화면(마이크 버튼 제외)과 미러링 화면, 두 방향 모두 검사한다.
  const int sizes[][2] = {{268, 208}, {240, 244}, {320, 208}, {240, 288}};
  for (const auto &size : sizes) {
    // 드라이브 수에 따라 칸 크기가 바뀌어도 한 페이지의 항목은 바닥 줄 위에 있다.
    for (int count = 0; count <= 26; ++count) {
      const auto l = PcStatusLayout::fit(size[0], size[1], count);
      assert(3 * l.card_width + 12 <= l.width);
      assert(l.card_width >= 70);
      assert(l.card_height >= 64);
      assert(l.card_y + l.card_height <= l.disk_heading_y);
      assert(l.disk_heading_y + 24 <= l.disk_y);
      assert(l.disk_slots >= 2);
      assert(l.disk_height + 2 <= l.disk_stride);
      assert(l.footer_y + 16 <= l.height);
      int visible = count < 1 ? 1 : count < l.disk_slots ? count : l.disk_slots;
      assert(l.disk_y + (visible - 1) * l.disk_stride + l.disk_height <= l.footer_y - 6);
    }
    // 드라이브가 하나면 남는 높이로 글자를 키운다.
    const auto one = PcStatusLayout::fit(size[0], size[1], 1);
    assert(one.card_height >= PcStatusLayout::BIG_CARD);
    assert(one.disk_height == PcStatusLayout::BIG_DISK);
  }
}
