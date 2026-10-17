#pragma once

#include <Arduino.h>

// 동영상 녹화. 카메라가 찍는 640x480 JPEG 를 한 장씩 PC 로 보내고, PC 도우미가 MOV 파일로 묶어 저장한다.
// SD 카드가 있으면(설정에서 켜 두면) 카드에도 같은 MOV 로 저장한다 (sd_video.h). 카드만 있어도 녹화할 수 있다.
// 원본 JPEG 를 다시 압축하지 않으며, 바로 보이게 돌리는 정보는 파일에 따로 적는다.
// 보내는 방법:
//   USB: USB_FRAME_EVENT 알림 + USB_FRAME_VIDEO 조각 [장 번호 4][위치 4][장 크기 4][시각 ms 4][데이터]
//   Wi-Fi: PC 의 TCP 4011 번으로 접속해 USB 와 같은 묶음 형식으로 보낸다
//   블루투스: 너무 느려서 녹화할 수 없다
// 알림: {"video": {"start": "VID_...mov", "turns": 1}} ... {"video": {"end": "VID_...mov", "frames": N, "dropped": M}}
#define VIDEO_TCP_PORT 4011

void video_init();

// 녹화를 시작한다. 시작하지 못하면 화면에 보여줄 이유(한글)를, 시작하면 nullptr 를 돌려준다.
const char *video_start(uint8_t turns);
// 카메라가 찍은 한 장을 넘긴다 (복사해 두고 바로 돌아온다).
void video_frame(const uint8_t *jpg, size_t len);
// 녹화를 끝낸다. 화면에 보여줄 결과(한글)를 돌려준다.
const char *video_stop();

bool video_recording();
// PC 로만 저장하는 중인지 (그러면 PC 연결이 끊길 때 녹화를 멈춘다)
bool video_needs_pc();
uint32_t video_elapsed_ms();
