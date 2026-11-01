#pragma once
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ORCA 세션 팝업 버튼의 배치. PC 가 보낸 {"pop": [[{"id", "name", "s"}, ...], ...]} 를 담는다 (orcapanel.cpp).
// LVGL 없이 판단하는 부분만 여기에 둔다 (firmware/tests/orca_pop_test.cpp).

#define POP_MAX 20  // 키워드 7개 (pcpanel 의 KW_MAX) + 리슘, proto, 지우기, 스킬 + 맨 아래 줄 5개 + 새로 만든 버튼 (companion/popbuttons.py 의 MAX_BUTTONS)
#define POP_ROWS 3
#define POP_DICTATE_COLOR 0x7A2E2A
#define POP_SLASH_COLOR 0x16808C  // 청록: pcpanel 의 KW_SLASH_COLOR
#define POP_KEYWORD_COLOR 0x6A4FB0  // 보라: pcpanel 의 KW_COLOR
#define POP_TABLE_VERSION 1
#define POP_TEXT_MAX 700  // 입력 글: 200자 (companion/typer.py 의 MAX_TEXT, 한글 3바이트) + 끝

struct PopItem {
  char id[24];    // "skill:proto-design". 보내는 JSON 에 그대로 넣는다 (pop_id_ok)
  char name[40];  // 12글자 (한글 3바이트)
  bool slash;     // 입력 글이 / 로 시작한다
};

// NVS 에 그대로 저장한다
struct PopTable {
  uint8_t version;
  uint8_t rows[POP_ROWS];  // 줄마다 버튼 수. item 에 줄 순서대로 있다
  PopItem item[POP_MAX];
};

// 위치 조정 결과를 보드에 저장하는 글 "a,b;c;d" (줄은 ;, 버튼은 ,). 가장 긴 id 20개도 들어간다
#define POP_LAYOUT_MAX 512
static_assert(POP_MAX * sizeof(PopItem::id) + POP_ROWS < POP_LAYOUT_MAX, "layout text");

inline bool pop_id_ok(const char *id) {
  size_t n = strlen(id);
  if (n == 0 || n >= sizeof(PopItem::id)) return false;
  for (size_t i = 0; i < n; i++) {
    if (!isalnum((unsigned char)id[i]) && !strchr("_:-.", id[i])) return false;
  }
  return true;
}

// id 로 고정된 색을 먼저 정한다. 그 외에는 / 명령이면 청록, 아니면 키워드 보라
inline uint32_t pop_color(const char *id, bool slash) {
  if (strcmp(id, "dictate") == 0) return POP_DICTATE_COLOR;
  if (strcmp(id, "tab") == 0 || strcmp(id, "enter") == 0 || strcmp(id, "esc") == 0) return 0x3A4350;
  if (strcmp(id, "tab_enter") == 0) return 0x5B6573;
  if (strcmp(id, "erase") == 0 || strcmp(id, "skills") == 0) return POP_SLASH_COLOR;
  return slash ? POP_SLASH_COLOR : POP_KEYWORD_COLOR;
}

// Tab, Esc, 청록 버튼은 눌러도 팝업을 닫지 않는다. 이어서 다른 걸 누른다
inline bool pop_stays_open(const char *id, bool slash) {
  return strcmp(id, "tab") == 0 || strcmp(id, "esc") == 0 || pop_color(id, slash) == POP_SLASH_COLOR;
}

// 받은 표가 쓸 수 있는지: 버튼 수, id 형식, id 중복, 이름
inline bool pop_table_ok(const PopTable &t) {
  int n = 0;
  for (uint8_t r : t.rows) n += r;
  if (n > POP_MAX) return false;
  for (int i = 0; i < n; i++) {
    if (!pop_id_ok(t.item[i].id) || !t.item[i].name[0]) return false;
    for (int j = 0; j < i; j++) {
      if (strcmp(t.item[i].id, t.item[j].id) == 0) return false;
    }
  }
  return n > 0;
}

// 위치 조정 결과. ids 는 줄 순서대로, counts 는 줄마다 버튼 수. 버퍼가 모자라면 false
inline bool pop_layout_text(char *out, size_t size, const char *const *ids, const int *counts) {
  size_t used = 0;
  auto put = [&](const char *text) {
    size_t n = strlen(text);
    if (used + n >= size) return false;
    memcpy(out + used, text, n + 1);
    used += n;
    return true;
  };
  out[0] = 0;
  int at = 0;
  for (int r = 0; r < POP_ROWS; r++) {
    if (r && !put(";")) return false;
    for (int k = 0; k < counts[r]; k++) {
      if ((k && !put(",")) || !put(ids[at++])) return false;
    }
  }
  return true;
}

// 버튼마다 고친 표시명('n')과 입력 글('t')의 NVS 키. NVS 키는 15자까지라 id 를 해시한다
inline void pop_nvs_key(char *out, size_t size, char kind, const char *id) {
  uint32_t h = 2166136261u;
  for (; *id; id++) h = (h ^ (uint8_t)*id) * 16777619u;
  snprintf(out, size, "%c%08lx", kind, (unsigned long)h);
}

struct PopRect {
  int x1, y1, x2, y2;
};

// 줄 안 버튼들 (왼쪽 위부터 줄바꿈 순서) 사이에서 (x, y) 에 놓으면 몇 번째로 들어가는지
inline int pop_drop_index(const PopRect *btns, int n, int x, int y) {
  for (int i = 0; i < n; i++) {
    if (y < btns[i].y1) return i;  // 이 버튼의 위 칸이다 (앞 칸 끝에 들어간다)
    if (y <= btns[i].y2 && x < (btns[i].x1 + btns[i].x2) / 2) return i;
  }
  return n;
}

// 세로 위치에서 가장 가까운 줄
inline int pop_drop_row(const PopRect *rows, int n, int y) {
  int best = 0, best_d = -1;
  for (int r = 0; r < n; r++) {
    int d = y < rows[r].y1 ? rows[r].y1 - y : y > rows[r].y2 ? y - rows[r].y2 : 0;
    if (best_d < 0 || d < best_d) best = r, best_d = d;
  }
  return best;
}
