#pragma once

// PC 상태 패널 내부 좌표. 가로/세로/미러링 모두 같은 구성, 드라이브가 많으면 버튼으로 넘긴다.
// 디스크 줄이 적어 아래가 남으면 그만큼 디스크 줄(큰 글자)과 사용률 칸(큰 글자)을 키워 화면을 채운다.
struct PcStatusLayout {
  static constexpr int BIG_CARD = 78;  // 이 높이부터 사용률 칸에 큰 글자 (이름 20, 값 28)
  static constexpr int BIG_DISK = 28;  // 디스크 줄 높이가 이만하면 큰 글자 (20)
  int width, height;
  int card_width, card_y = 26, card_height = 64;
  int disk_heading_y = 94, disk_y = 120, disk_stride = 24, disk_height = 22;
  int disk_slots, footer_y;

  static PcStatusLayout fit(int w, int h, int disks) {
    PcStatusLayout l{};
    l.width = w - 16;
    l.height = h - 16;
    l.card_width = (l.width - 12) / 3;
    l.footer_y = l.height - 16;
    l.disk_slots = (l.footer_y - 6 - l.disk_y) / l.disk_stride;
    if (l.disk_slots < 1) l.disk_slots = 1;
    if (l.disk_slots > 26) l.disk_slots = 26;
    int rows = disks < 1 ? 1 : disks > l.disk_slots ? l.disk_slots : disks;
    int spare = l.footer_y - 6 - (l.disk_y + (rows - 1) * l.disk_stride + l.disk_height);
    // 먼저 디스크 줄을 큰 글자로, 남은 것은 사용률 칸, 그래도 남으면 디스크 줄 간격
    int grow = (rows - 1) * 8 + (BIG_DISK - l.disk_height);
    if (spare >= grow) {
      spare -= grow;
      l.disk_stride += 8;
      l.disk_height = BIG_DISK;
    }
    int card = spare < 40 ? spare : 40;
    l.card_height += card;
    spare -= card;
    if (rows > 1) {
      int gap = spare / (rows - 1);
      l.disk_stride += gap < 8 ? gap : 8;
    }
    l.disk_heading_y = l.card_y + l.card_height + 4;
    l.disk_y = l.disk_heading_y + 26;
    return l;
  }
};
