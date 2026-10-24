#pragma once

// 카메라 화면. 홈의 카메라 아이콘으로 열고, 오른쪽으로 밀면 홈으로(아래로 밀면 닫기).
// 미리보기를 누르면 사진을 찍어 SD 카드와 PC 에 저장한다 (photo.h).
void camera_ui_init();
void camera_ui_show();
void camera_ui_loop();
// 화면 방향이 바뀌었을 때
void camera_ui_layout();
bool camera_ui_open();
// PC 에서 카메라 화면을 다룬다: "open" / "shoot" / "close" / "record" / "gallery"
// 처리 실패(카메라 없음, 녹화 중 촬영 등)는 false. 파일 전송 완료는 별도 알림이다.
bool camera_ui_command(const char *cmd);
// 연속 스캔 중 PC 가 QR·바코드를 읽었을 때 ({"scan": "바코드: 880..."}, companion/photos.py): 결과를 보이고 스캔을 멈춘다
void camera_ui_scan_result(const char *text);
// 스캔 1단계 장에서 PC 가 찾은 바코드 자리 ({"scan_roi": [x0, y0, x1, y1]}, 장 기준 0~1000). found 가 false 면 못 찾음
void camera_ui_scan_roi(const int *box, bool found);
