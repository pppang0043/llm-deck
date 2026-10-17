#pragma once

#include <Arduino.h>

// SD 카드에 동영상을 MOV(Motion JPEG) 파일로 바로 쓴다. PC 도우미(companion/videos.py)와 같은 형식이다.
// 카메라가 준 JPEG 를 다시 압축하지 않고 이어 붙이며, 바로 보이게 돌리는 정보와 장마다 찍힌 시각을 파일 끝에 적는다.
// 카드 쓰기는 따로 도는 태스크에서 한다 (화면과 같은 SPI 선을 잠금으로 번갈아 쓴다).

// 녹화 시작. 카드가 없거나 파일을 못 만들면 false.
bool sd_video_start(const char *name, uint8_t turns);
// 카메라 태스크에서 불러도 된다 (복사해 두고 바로 돌아온다). 쓰기가 밀리면 그 장은 버린다.
void sd_video_frame(const uint8_t *jpg, size_t len, uint32_t ms);
// 남은 장을 다 쓰고 파일을 닫는다 (loop 에서 마무리).
void sd_video_stop();
// 쓰는 태스크를 만든다 (loop 에서 불러도 되고, 녹화 시작 때 알아서 부른다)
void sd_video_loop();

bool sd_video_active();
// 마지막 녹화 결과: 쓴 장 수, 버린 장 수, 실패했는지
void sd_video_result(uint32_t *frames, uint32_t *dropped, bool *failed);
