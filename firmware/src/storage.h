#pragma once

#include <Arduino.h>

// 보드의 microSD 카드. 필요할 때 연결(마운트)하고, 빠지면 다음에 다시 시도한다.
bool storage_ready();
// 파일을 쓴다. path 는 카드 안의 경로 (예: "/photos/IMG_1.jpg"). 폴더는 알아서 만든다.
bool storage_write(const char *path, const uint8_t *data, size_t len);

// 설정 화면용
// 카드 크기·사용량 (MB). 카드가 없으면 false.
bool storage_info(uint32_t *total_mb, uint32_t *used_mb);
// 폴더 안의 파일 수 (카드가 없으면 -1)
int storage_count(const char *dir);
// 폴더 안의 파일을 모두 지운다. 지운 개수를 돌려준다.
int storage_clear(const char *dir);
// 카드를 다시 찾는다 (방금 꽂았을 때). 10초 기다림 없이 바로.
bool storage_recheck();
// 읽을 수 없는 카드(exFAT 등)를 FAT32 로 포맷한다. 그 카드 안의 내용은 모두 지워진다. 읽히는 카드는 그대로 둔다.
bool storage_format();
// 폴더에서 이름이 가장 뒤인(가장 최근) 파일을 PSRAM 으로 읽는다 (진단용). *data 는 free 로 해제.
bool storage_read_last(const char *dir, char *name, size_t name_size, uint8_t **data, size_t *len);
