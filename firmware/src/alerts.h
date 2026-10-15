#pragma once

#include <ArduinoJson.h>

// 한도 알림: 새 사용량을 받을 때마다 지난번 값과 비교해, 사용률이 80% 나 90% 를 넘었거나
// 한도가 초기화되었으면 화면에 알림을 띄우고 연결된 PC 스피커로 소리를 낸다.
// 처음 보는 구간(켠 직후 등)은 비교할 값이 없으므로 알리지 않는다.
void alerts_check(JsonArrayConst services);
