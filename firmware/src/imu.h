#pragma once

#include <Arduino.h>

// 기울기 센서(QMI8658). 화면과 같은 I2C 선을 쓰므로 display_init() 뒤에 불러야 한다.
void imu_init();

// 기기가 세워진 방향을 화면 회전값(0~3, 90도 단위)으로 돌려준다.
// 눕혀 놓았거나 애매하게 기울어 있으면 -1.
int imu_rotation();

// 가속도 값(g 단위). 센서가 없으면 false.
bool imu_read(float *x, float *y, float *z);
