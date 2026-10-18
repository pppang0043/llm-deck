#pragma once

// 갤러리: SD 카드의 사진(/photos)과 동영상(/videos)을 최근 것부터 한 장씩 본다.
// 카메라 화면 오른쪽 위 버튼으로 연다. 왼쪽으로 밀면 이전(더 오래된) 것, 오른쪽으로 밀면 다음(더 최근) 것, 아래로 밀면 닫는다.
// 동영상은 첫 장이 보이고, 화면을 누르면 재생·멈춤. 오른쪽 아래 휴지통을 3초 안에 두 번 누르면 카드에서 지운다.
void gallery_init();
// on_close: 닫을 때 부를 것 (카메라 화면으로 돌아가기 등). 없으면 nullptr.
void gallery_show(void (*on_close)());
void gallery_loop();
void gallery_layout();
bool gallery_open();
