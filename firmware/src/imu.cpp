#include "imu.h"

#include <Wire.h>

#include "pins.h"

#define QMI8658_ADDR 0x6B
#define REG_WHO_AM_I 0x00
#define REG_CTRL1 0x02
#define REG_CTRL2 0x03
#define REG_CTRL7 0x08
#define REG_ACCEL 0x35
// +-4g 범위에서 1g 에 해당하는 값
#define LSB_PER_G 8192.0f

// 이 축으로 이만큼(g) 이상 기울어야 세운 것으로 본다
#define TILT_ON 0.6f
// 다른 축은 이보다 작아야 한다 (대각선으로 들고 있을 때 오락가락하지 않도록)
#define TILT_OFF 0.45f

static bool present = false;

static bool write_reg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(QMI8658_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool read_regs(uint8_t reg, uint8_t *data, uint8_t len) {
  Wire.beginTransmission(QMI8658_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)QMI8658_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) data[i] = Wire.read();
  return true;
}

void imu_init() {
  uint8_t id = 0;
  if (!read_regs(REG_WHO_AM_I, &id, 1) || id != 0x05) {
    Serial.printf("[imu] sensor not found (id 0x%02X)\n", id);
    return;
  }
  write_reg(REG_CTRL1, 0x40);  // 여러 바이트를 이어서 읽을 수 있게
  write_reg(REG_CTRL2, 0x17);  // 가속도 +-4g, 초당 약 60회
  write_reg(REG_CTRL7, 0x01);  // 가속도만 켠다
  present = true;
  Serial.println("[imu] ready");
}

bool imu_read(float *x, float *y, float *z) {
  uint8_t raw[6];
  if (!present || !read_regs(REG_ACCEL, raw, sizeof(raw))) return false;
  *x = (int16_t)(raw[0] | raw[1] << 8) / LSB_PER_G;
  *y = (int16_t)(raw[2] | raw[3] << 8) / LSB_PER_G;
  *z = (int16_t)(raw[4] | raw[5] << 8) / LSB_PER_G;
  return true;
}

int imu_rotation() {
  float x, y, z;
  if (!imu_read(&x, &y, &z)) return -1;
  // 센서가 보드에 붙은 방향에 따른 대응. 중력이 향하는 쪽이 화면의 아래가 된다.
  if (fabsf(y) > TILT_ON && fabsf(x) < TILT_OFF) return y > 0 ? IMU_ROT_Y_POS : IMU_ROT_Y_NEG;
  if (fabsf(x) > TILT_ON && fabsf(y) < TILT_OFF) return x > 0 ? IMU_ROT_X_POS : IMU_ROT_X_NEG;
  return -1;
}
