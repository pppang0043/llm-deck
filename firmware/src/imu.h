#pragma once

#include <Arduino.h>

// 센서 INT1 이 연결된 핀 (RTC 핀이라 깊은 잠에서 깨울 수 있다, pins.h 참고)
#define IMU_INT1 3

// 기울기 센서(QMI8658). 화면과 같은 I2C 선을 쓰므로 display_init() 뒤에 불러야 한다.
void imu_init();

// 기기가 세워진 방향을 화면 회전값(0~3, 90도 단위)으로 돌려준다.
// 눕혀 놓았거나 애매하게 기울어 있으면 -1.
int imu_rotation();

// 가속도 값(x, y)으로 본 방향. imu_rotation() 과 같지만 이미 읽은 값을 쓴다.
int imu_rotation_of(float x, float y);

// 가속도 값(g 단위). 센서가 없으면 false.
bool imu_read(float *x, float *y, float *z);
// 회전 속도(도/초). 센서가 없으면 false.
bool imu_read_gyro(float *x, float *y, float *z);

// 깊은 잠 직전에 부른다: 자이로를 끄고 가속도 저전력 + 움직임 감지(WoM)로 바꿔, 움직이면 INT1 을 HIGH 로 올린다.
// 깨어나 imu_init() 을 부르면 센서를 처음 상태로 되돌린다. 센서가 없거나 설정에 실패하면 false.
bool imu_wake_on_motion();
