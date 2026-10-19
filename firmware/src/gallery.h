#pragma once

// 갤러리: SD 카드의 사진(/photos)과 동영상(/videos). 카메라 화면 오른쪽 위 버튼으로 연다.
// 먼저 작은 그림 격자(최근 것부터, 위 가운데 버튼으로 전체/사진/동영상)가 나오고, 누르면 크게 본다.
// 크게 보기: 왼쪽으로 밀면 더 오래된 것, 오른쪽으로 밀면 더 최근 것, 아래로 밀거나 왼쪽 위 버튼으로 격자로.
// 동영상은 화면을 누르면 재생·멈춤. 오른쪽 아래 휴지통을 3초 안에 두 번 누르면 카드에서 지우고,
// 왼쪽 아래 버튼은 이름 그대로 PC 로 다시 보낸다 (동영상은 USB 케이블로 연결한 PC 로만).
// 격자의 작은 그림은 처음 그릴 때 카드의 /thumbs 에 저장해 두고 다음부터 그것을 쓴다.
// 녹화 중에 전원이 꺼져 끝나지 않은 동영상은 열 때 고친다 (mov_repair.h).
void gallery_init();
// on_close: 닫을 때 부를 것 (카메라 화면으로 돌아가기 등). 없으면 nullptr.
void gallery_show(void (*on_close)());
void gallery_loop();
void gallery_layout();
bool gallery_open();
