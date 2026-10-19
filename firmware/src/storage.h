#pragma once

#include <Arduino.h>

// 보드의 microSD 카드. 필요할 때 연결(마운트)하고, 빠지면 다음에 다시 시도한다.
bool storage_ready();
// 파일을 쓴다. path 는 카드 안의 경로 (예: "/photos/IMG_1.jpg"). 폴더는 알아서 만든다.
bool storage_write(const char *path, const uint8_t *data, size_t len);

// 설정 화면용
// 카드 크기·사용량 (MB). 카드가 없으면 false.
bool storage_info(uint32_t *total_mb, uint32_t *used_mb);
// 남은 공간 (바이트, 카드가 없으면 -1). 처음 한 번은 수 초 걸릴 수 있다 (loop 에서 부른다).
// 그 뒤로는 기억해 둔 값에서 쓴 만큼 뺀 대략의 값이다.
int64_t storage_free_bytes();
// 카드에 이만큼 썼다 / 지웠다 (남은 공간 값을 고친다. 어느 태스크에서 불러도 된다)
void storage_note_written(size_t len);
void storage_note_removed();
// 남은 공간이 50MB 또는 5% 아래인지. *free_mb 에 남은 MB.
#define STORAGE_LOW_BYTES (50ULL * 1024 * 1024)
bool storage_space_low(uint32_t *free_mb = nullptr);
// 공간이 얼마 안 남았으면 알림을 띄운다 (1분에 한 번만, force 면 바로). 화면을 그리는 loop 에서만 부른다.
void storage_warn_low(bool force = false);
// 폴더 안의 파일 수 (카드가 없으면 -1)
int storage_count(const char *dir);
// 폴더 안의 파일을 모두 지운다 (그 파일들의 갤러리 작은 그림도). 지운 개수를 돌려준다.
// 동영상을 마저 쓰는 중이면 그 파일은 남긴다.
int storage_clear(const char *dir);
// 카드가 아직 꽂혀 있는지 실제로 읽어 확인한다 (빠졌으면 연결을 끊고 바로 다시 찾는다). 갤러리를 열 때처럼 가끔만.
bool storage_present();
// 갤러리 격자의 작은 그림 파일: "/photos/IMG_1.jpg" -> "/thumbs/IMG_1.jpg.rgb"
#define STORAGE_THUMBS "/thumbs"
String storage_thumb_path(const String &path);
// 카드를 다시 찾는다 (방금 꽂았을 때). 10초 기다림 없이 바로.
bool storage_recheck();
// 읽을 수 없는 카드(exFAT 등)를 FAT32 로 포맷한다. 그 카드 안의 내용은 모두 지워진다. 읽히는 카드는 그대로 둔다.
bool storage_format();
// 폴더에서 이름이 가장 뒤인(가장 최근) 파일을 PSRAM 으로 읽는다 (진단용). *data 는 free 로 해제.
bool storage_read_last(const char *dir, char *name, size_t name_size, uint8_t **data, size_t *len);
