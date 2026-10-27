#pragma once

// PC 상태 패널 내부 좌표. 가로/세로/미러링 모두 같은 고정 구성, 드라이브만 버튼으로 넘긴다.
struct PcStatusLayout {
  int width, height;
  int card_width, card_y = 26, card_height = 64;
  int disk_heading_y = 94, disk_y = 120, disk_stride = 24, disk_height = 22;
  int disk_slots, footer_y;

  static PcStatusLayout fit(int w, int h) {
    PcStatusLayout l{};
    l.width = w - 16;
    l.height = h - 16;
    l.card_width = (l.width - 12) / 3;
    l.footer_y = l.height - 16;
    l.disk_slots = (l.footer_y - 6 - l.disk_y) / l.disk_stride;
    if (l.disk_slots < 1) l.disk_slots = 1;
    if (l.disk_slots > 26) l.disk_slots = 26;
    return l;
  }
};
