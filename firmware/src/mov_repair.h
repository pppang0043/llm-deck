#pragma once

#include <Arduino.h>

// 녹화 중에 전원이 꺼져 끝나지 않은 동영상(MOV)을 고친다: 목차(moov)가 없고 mdat 크기가 0 인 파일.
// mdat 안을 처음부터 읽으며 JPEG 한 장의 시작(FFD8)과 끝(FFD9)을 찾아 목차를 파일 끝에 붙이고,
// 마지막에 mdat 크기를 고친다. 도중에 실패해도 원래 장들은 그대로 남는다 (다시 시도할 수 있다).
// 장마다의 시각은 남아 있지 않아 1초에 10장으로, 화면 방향은 돌리지 않은 것으로 적는다.
// progress: 진행률(%)을 받을 함수 (화면을 다시 그리는 등). 없으면 nullptr. 큰 파일은 수십 초 걸린다.
// 고친 장 수를 돌려준다 (0 이면 고치지 못함).
uint32_t mov_repair(const char *path, void (*progress)(int pct));
