#pragma once

#include <Arduino.h>

void battery_init();

// loop() 에서 불러준다. 몇 초마다 전압을 재서 잔량을 갱신한다.
void battery_loop();

// 배터리 전압 (mV). 배터리가 없으면 0.
int battery_mv();

// 남은 양 (0~100). 배터리가 없으면 -1.
int battery_pct();

// 멀티미터로 잰 실제 배터리 전압(mV)을 알려 주면 측정값을 그에 맞게 보정하고 기억한다. 0 이면 보정을 지운다.
void battery_calibrate(int actual_mv);

// 충전기(또는 PC 의 USB)가 연결되어 충전 중인지. 충전이 끝나면 false (잔량은 100).
bool battery_charging();
// 충전기나 PC 의 USB 로 전원이 들어오고 있는지 (충전이 끝났어도 true)
bool battery_external_power();
