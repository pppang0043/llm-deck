#include "orca_pop.h"
#include <assert.h>

int main() {
  // 눌러도 팝업이 남는 버튼은 지금과 같다
  assert(pop_stays_open("tab", false));
  assert(pop_stays_open("esc", false));
  assert(pop_stays_open("erase", false));
  assert(pop_stays_open("clear", true));
  assert(pop_stays_open("resume", true));
  assert(pop_stays_open("skill:proto-design", true));
  assert(!pop_stays_open("commit", false));
  assert(!pop_stays_open("enter", true));  // id 로 정한 색이 s 보다 먼저다
  assert(!pop_stays_open("tab_enter", false));
  assert(pop_color("dictate", true) == POP_DICTATE_COLOR);
  assert(pop_color("commit", false) == POP_KEYWORD_COLOR);

  // id 는 JSON 에 그대로 들어간다
  assert(pop_id_ok("skill:proto-design"));
  assert(pop_id_ok("k0"));
  assert(!pop_id_ok(""));
  assert(!pop_id_ok("a\"b"));
  assert(!pop_id_ok("a\\b"));
  assert(!pop_id_ok("aaaaaaaaaaaaaaaaaaaaaaaa"));  // 24자

  // 받은 표 검사
  PopTable t;
  memset(&t, 0, sizeof(t));
  assert(!pop_table_ok(t));  // 버튼이 없다
  t.rows[1] = 2;
  strcpy(t.item[0].id, "a");
  strcpy(t.item[0].name, "A");
  strcpy(t.item[1].id, "a");
  strcpy(t.item[1].name, "B");
  assert(!pop_table_ok(t));  // 같은 id
  strcpy(t.item[1].id, "b");
  assert(pop_table_ok(t));
  t.item[1].name[0] = 0;
  assert(!pop_table_ok(t));  // 이름 없음
  strcpy(t.item[1].name, "B");
  t.rows[0] = 15;
  assert(!pop_table_ok(t));  // 17개

  // 위치 글: 줄은 ;, 버튼은 , (빈 줄은 비어 있다)
  const char *const ids[POP_MAX] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t"};
  int counts[POP_ROWS] = {2, 0, 18};
  char text[POP_LAYOUT_MAX];
  assert(pop_layout_text(text, sizeof(text), ids, counts));
  assert(strcmp(text, "a,b;;c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t") == 0);
  // 가장 긴 id 20개도 잘리지 않고, 버퍼가 모자라면 false
  const char *longest = "aaaaaaaaaaaaaaaaaaaaaaa";  // 23자
  const char *const longs[POP_MAX] = {longest, longest, longest, longest, longest, longest, longest, longest, longest, longest,
                                      longest, longest, longest, longest, longest, longest, longest, longest, longest, longest};
  int all[POP_ROWS] = {0, 20, 0};
  assert(pop_layout_text(text, sizeof(text), longs, all));
  char small[8];
  assert(!pop_layout_text(small, sizeof(small), ids, counts));

  // NVS 키는 15자 안이고, 종류와 id 로 갈린다
  char k1[16], k2[16], k3[16];
  pop_nvs_key(k1, sizeof(k1), 'n', "skill:proto-design");
  pop_nvs_key(k2, sizeof(k2), 't', "skill:proto-design");
  pop_nvs_key(k3, sizeof(k3), 'n', "commit");
  assert(strlen(k1) == 9 && strcmp(k1, k2) != 0 && strcmp(k1, k3) != 0);

  // 줄 둘로 접힌 버튼들: 위 칸 (0, 1), 아래 칸 (2)
  const PopRect b[3] = {{0, 0, 10, 10}, {14, 0, 24, 10}, {0, 14, 10, 24}};
  assert(pop_drop_index(b, 3, 2, 5) == 0);
  assert(pop_drop_index(b, 3, 8, 5) == 1);
  assert(pop_drop_index(b, 3, 20, 5) == 2);  // 위 칸의 끝
  assert(pop_drop_index(b, 3, 8, 12) == 2);  // 칸 사이 틈
  assert(pop_drop_index(b, 3, 2, 20) == 2);
  assert(pop_drop_index(b, 3, 8, 20) == 3);  // 맨 끝
  assert(pop_drop_index(b, 3, 100, 100) == 3);
  assert(pop_drop_index(b, 0, 5, 5) == 0);  // 빈 줄

  const PopRect rows[3] = {{0, 10, 100, 20}, {0, 24, 100, 34}, {0, 38, 100, 48}};
  assert(pop_drop_row(rows, 3, 0) == 0);
  assert(pop_drop_row(rows, 3, 15) == 0);
  assert(pop_drop_row(rows, 3, 30) == 1);
  assert(pop_drop_row(rows, 3, 44) == 2);
  assert(pop_drop_row(rows, 3, 300) == 2);

  // 입력 글 조각: 한글(3바이트) 중간에서 자르지 않고, 이어 붙이면 원래 글이다
  const char *ko = "a가나다라";  // 1 + 3 * 4 바이트
  size_t len = strlen(ko);
  assert(pop_text_part(ko, len, 0, 5) == 4);    // "a가"
  assert(pop_text_part(ko, len, 4, 5) == 3);    // "나"
  assert(pop_text_part(ko, len, 10, 60) == 3);  // 마지막 "라"
  char joined[32] = "";
  for (size_t at = 0, n; at < len; at += n) {
    n = pop_text_part(ko, len, at, 4);
    assert(n > 0);
    strncat(joined, ko + at, n);
  }
  assert(strcmp(joined, ko) == 0);
  return 0;
}
