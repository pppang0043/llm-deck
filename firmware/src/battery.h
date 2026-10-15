#pragma once

#include <Arduino.h>

void battery_init();

// loop() 에서 불러준다. 몇 초마다 전압을 재서 잔량을 갱신한다.
void battery_loop();

// 배터리 전압 (mV). 배터리가 없으면 0.
int battery_mv();

// 남은 양 (0~100). 배터리가 없으면 -1.
int battery_pct();

// 충전기(또는 PC 의 USB)가 연결되어 충전 중인지
bool battery_charging();
