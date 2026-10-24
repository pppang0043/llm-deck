#include "speaker.h"

#include <Wire.h>

#include "settings.h"
#include "speaker_proto.h"

// 모듈이 다시 시작하는 동안(0.5초 정도)은 대답이 없으므로 몇 번 연달아 실패해야 없어진 것으로 본다
#define MISS_LIMIT 4
#define POLL_MS 2000
#define POLL_FAST_MS 500
// 모듈이 없을 때 찾아보는 간격 (스피커 화면에서는 자주)
#define PROBE_MS 30000
#define PROBE_FAST_MS 5000

static bool present = false;
static int misses = 0;
static uint8_t status[SPK_REPLY_LEN];

struct FoundEntry {
  bool used;
  int8_t rssi;
  char name[SPK_NAME_LEN];
};
static FoundEntry found[SPK_MAX_FOUND];
static int found_count = 0;

static bool send(const uint8_t *data, size_t len) {
  Wire.beginTransmission(SPK_ADDR);
  Wire.write(data, len);
  return Wire.endTransmission(true) == 0;
}

// 고른 내용을 읽는다. 모듈이 답을 미리 넣을 틈을 주려고 잠깐 기다린다.
static bool read_reply(uint8_t select, uint8_t *buf) {
  uint8_t cmd[2] = {SPK_CMD_SELECT, select};
  if (!send(cmd, 2)) return false;
  delay(3);
  if (Wire.requestFrom((uint8_t)SPK_ADDR, (uint8_t)SPK_REPLY_LEN) != SPK_REPLY_LEN) return false;
  for (int i = 0; i < SPK_REPLY_LEN; i++) buf[i] = Wire.read();
  return true;
}

// 주소만 불러서 대답하는지 본다. 데이터를 보내면 없는 주소일 때 코어가 I2C 오류 줄
// (i2c_master_transmit failed: ESP_ERR_INVALID_STATE)을 찍지만, 빈 전송은 조용히 확인만 한다.
static bool answers() {
  Wire.beginTransmission(SPK_ADDR);
  return Wire.endTransmission(true) == 0;
}

static bool poll_status() {
  uint8_t buf[SPK_REPLY_LEN];
  if (!present && !answers()) return false;
  if (!read_reply(0xFF, buf) || buf[0] != 'A' || buf[1] != SPK_PROTO) {
    if (present && ++misses >= MISS_LIMIT) {
      present = false;
      found_count = 0;
      Serial.println("[speaker] module gone");
    }
    return false;
  }
  misses = 0;
  if (!present) {
    present = true;
    Serial.printf("[speaker] module found (state %u, target %s)\n", buf[2], buf[6] ? (char *)buf + 8 : "-");
  }
  memcpy(status, buf, sizeof(status));
  status[SPK_REPLY_LEN - 1] = 0;
  return true;
}

// 설정(항상 연결, 소리 크기)은 보드가 정하고 모듈에 맞춰 준다. 모듈도 저장해 두지만 보드 쪽이 기준이다.
static void sync_config() {
  bool keep = settings_speaker_keep();
  uint8_t vol = settings_speaker_volume();
  if (status[4] == keep && status[5] == vol) return;
  uint8_t cmd[3] = {SPK_CMD_CONFIG, keep, vol};
  if (send(cmd, 3)) {
    status[4] = keep;
    status[5] = vol;
  }
}

static void read_found() {
  int n = status[3] > SPK_MAX_FOUND ? SPK_MAX_FOUND : status[3];
  int count = 0;
  for (int i = 0; i < n; i++) {
    uint8_t buf[SPK_REPLY_LEN];
    if (!read_reply(i, buf) || buf[0] != 'E') return;
    FoundEntry &f = found[i];
    f.used = buf[1];
    f.rssi = (int8_t)buf[2];
    memcpy(f.name, buf + 4, SPK_NAME_LEN);
    f.name[SPK_NAME_LEN - 1] = 0;
    if (f.used) count = i + 1;
  }
  found_count = count;
}

void speaker_init() {
  // Wire 는 display_init 에서 이미 켰다 (터치 센서와 같은 선)
  poll_status();
  if (present) sync_config();
}

void speaker_loop(bool fast) {
  static uint32_t polled_ms = 0;
  uint32_t every = !present ? (fast ? PROBE_FAST_MS : PROBE_MS) : fast ? POLL_FAST_MS : POLL_MS;
  if (millis() - polled_ms < every) return;
  polled_ms = millis();
  if (!poll_status()) return;
  sync_config();
  // 찾은 목록은 스피커 화면을 볼 때만 읽는다
  if (fast) read_found();
}

bool speaker_present() {
  return present;
}

int speaker_state() {
  return present ? status[2] : -1;
}

bool speaker_target(char *name, size_t size) {
  if (!present || !status[6]) return false;
  if (name && size) strlcpy(name, (const char *)status + 8, size);
  return true;
}

static uint8_t sound_id(const char *sound) {
  if (strcmp(sound, "mic_on") == 0) return SPK_SOUND_MIC_ON;
  if (strcmp(sound, "mic_off") == 0) return SPK_SOUND_MIC_OFF;
  if (strcmp(sound, "limit_warn") == 0) return SPK_SOUND_LIMIT_WARN;
  if (strcmp(sound, "limit_reset") == 0) return SPK_SOUND_LIMIT_RESET;
  if (strcmp(sound, "test") == 0) return SPK_SOUND_TEST;
  return 0;
}

bool speaker_play(const char *sound) {
  uint8_t id = sound_id(sound);
  if (!id || !present || !status[6] || !settings_speaker_on()) return false;
  uint8_t cmd[2] = {SPK_CMD_PLAY, id};
  if (!send(cmd, 2)) return false;
  Serial.printf("[speaker] play %s\n", sound);
  return true;
}

void speaker_find() {
  if (!present) return;
  uint8_t cmd = SPK_CMD_FIND;
  send(&cmd, 1);
  found_count = 0;
}

int speaker_found_count() {
  return found_count;
}

bool speaker_found(int i, char *name, size_t size, int *rssi) {
  if (i < 0 || i >= found_count || !found[i].used) return false;
  if (name && size) strlcpy(name, found[i].name, size);
  if (rssi) *rssi = found[i].rssi;
  return true;
}

void speaker_connect(int i) {
  if (!present || i < 0 || i >= found_count) return;
  uint8_t cmd[2] = {SPK_CMD_CONNECT, (uint8_t)i};
  send(cmd, 2);
  found_count = 0;
}

void speaker_forget() {
  if (!present) return;
  uint8_t cmd = SPK_CMD_FORGET;
  send(&cmd, 1);
  status[6] = 0;
  status[8] = 0;
}
