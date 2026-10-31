#include "imu.h"

#include <Wire.h>

#include "pins.h"

#define QMI8658_ADDR 0x6B
#define REG_WHO_AM_I 0x00
#define REG_CTRL1 0x02
#define REG_CTRL2 0x03
#define REG_CTRL3 0x04
#define REG_CTRL7 0x08
#define REG_CTRL9 0x0A
#define REG_CAL1_L 0x0B
#define REG_CAL1_H 0x0C
#define REG_STATUSINT 0x2D
#define REG_ACCEL 0x35
#define REG_GYRO 0x3B
#define REG_RESET 0x60
// CTRL9 명령: 움직임 감지(WoM) 설정을 CAL1 에서 읽어 들인다, 0x00 은 완료 확인(ACK)
#define CMD_WRITE_WOM_SETTING 0x08
#define CMD_ACK 0x00
// +-4g 범위에서 1g 에 해당하는 값
#define LSB_PER_G 8192.0f
// +-512 도/초 범위에서 1 도/초에 해당하는 값 (32768 / 512)
#define LSB_PER_DPS 64.0f
// 깊은 잠에서 깨우는 움직임 크기 (mg, 1mg 단위). 책상 진동은 넘지 않고 들어 올리면 넘는 정도
#define WOM_THRESHOLD_MG 200

// 이 축으로 이만큼(g) 이상 기울어야 세운 것으로 본다 (눕혀 놓으면 두 축 모두 작다)
#define TILT_MIN 0.5f
// 두 축의 차이가 이보다 커야 한다 (대각선으로 들고 있을 때 오락가락하지 않도록)
#define TILT_MARGIN 0.2f

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

// CTRL9 명령을 보내고 끝날 때까지 기다린다 (STATUSINT bit7 = 명령 완료, ACK 를 보내면 내려간다)
static bool command(uint8_t cmd) {
  uint8_t status = 0;
  if (!write_reg(REG_CTRL9, cmd)) return false;
  uint32_t start = millis();
  while (!(read_regs(REG_STATUSINT, &status, 1) && (status & 0x80))) {
    if (millis() - start > 100) return false;
    delay(1);
  }
  write_reg(REG_CTRL9, CMD_ACK);
  start = millis();
  while (read_regs(REG_STATUSINT, &status, 1) && (status & 0x80)) {
    if (millis() - start > 100) return false;
    delay(1);
  }
  return true;
}

void imu_init() {
  uint8_t id = 0;
  // 깊은 잠 동안 센서는 켜진 채 움직임 감지(WoM) 상태로 남아 있으므로 처음 상태로 되돌린다 (최대 15ms)
  write_reg(REG_RESET, 0xB0);
  delay(20);
  if (!read_regs(REG_WHO_AM_I, &id, 1) || id != 0x05) {
    Serial.printf("[imu] sensor not found (id 0x%02X)\n", id);
    return;
  }
  write_reg(REG_CTRL1, 0x40);  // 여러 바이트를 이어서 읽을 수 있게
  write_reg(REG_CTRL2, 0x17);  // 가속도 +-4g, 초당 약 60회
  write_reg(REG_CTRL3, 0x57);  // 자이로 +-512도/초, 초당 약 56회 (둘 다 켜면 가속도도 이 속도)
  write_reg(REG_CTRL7, 0x03);  // 가속도와 자이로를 켠다 (자이로는 빨리 돌린 뒤 멈춘 것을 알아보는 데 쓴다)
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

bool imu_read_gyro(float *x, float *y, float *z) {
  uint8_t raw[6];
  if (!present || !read_regs(REG_GYRO, raw, sizeof(raw))) return false;
  *x = (int16_t)(raw[0] | raw[1] << 8) / LSB_PER_DPS;
  *y = (int16_t)(raw[2] | raw[3] << 8) / LSB_PER_DPS;
  *z = (int16_t)(raw[4] | raw[5] << 8) / LSB_PER_DPS;
  return true;
}

bool imu_wake_on_motion() {
  if (!present) return false;
  // 데이터시트의 WoM 순서: 센서를 멈추고, 가속도 설정, CAL1 에 값, CTRL9 명령, 가속도만 다시 켠다
  write_reg(REG_CTRL7, 0x00);
  write_reg(REG_CTRL1, 0x40);  // 설정하는 동안 INT1 은 끈다
  write_reg(REG_CTRL2, 0x1D);  // 가속도 +-4g, 저전력 21Hz (저전력은 자이로를 꺼야 쓸 수 있다)
  write_reg(REG_CAL1_L, WOM_THRESHOLD_MG);
  // [7:6] 00 = INT1 로, 처음 값 LOW (움직이면 HIGH 로 바뀐다) / [5:0] 켠 직후 32 표본(약 1.5초)은 무시
  write_reg(REG_CAL1_H, 0x20);
  if (!command(CMD_WRITE_WOM_SETTING)) {
    Serial.println("[imu] wake-on-motion setup failed");
    return false;
  }
  write_reg(REG_CTRL7, 0x01);  // 가속도만
  write_reg(REG_CTRL1, 0x48);  // INT1 출력 켜기
  return true;
}

int imu_rotation_of(float x, float y) {
  // 센서가 보드에 붙은 방향에 따른 대응. 중력이 향하는 쪽이 화면의 아래가 된다.
  float ax = fabsf(x), ay = fabsf(y);
  if (ay > TILT_MIN && ay - ax > TILT_MARGIN) return y > 0 ? IMU_ROT_Y_POS : IMU_ROT_Y_NEG;
  if (ax > TILT_MIN && ax - ay > TILT_MARGIN) return x > 0 ? IMU_ROT_X_POS : IMU_ROT_X_NEG;
  return -1;
}

int imu_rotation() {
  float x, y, z;
  if (!imu_read(&x, &y, &z)) return -1;
  return imu_rotation_of(x, y);
}
