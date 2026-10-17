// 블루투스 스피커 모듈 (M5Stack ATOM Lite)
// LLM Deck 보드에서 I2C 로 "이 소리 내 줘" 명령을 받아, 기억해 둔 블루투스 스피커(A2DP)로 소리를 보낸다.
// 소리(알림음)는 여기서 직접 만든다 (I2C 선은 느려서 소리 데이터를 실어 보내기에는 모자라다).
//
// 전기 아끼기: 블루투스는 소리를 낼 때만 켠다. 마지막 소리 뒤 30초가 지나면 다시 끈다.
// "항상 연결"을 켜면 계속 연결해 두어 소리가 바로 난다.
// 상태를 바꿀 때(쉬기 <-> 연결 <-> 찾기)는 모듈을 다시 시작해 블루투스를 깨끗하게 다시 켠다 (0.5초 정도).

#include <Arduino.h>
#include <BluetoothA2DPSource.h>
#include <Preferences.h>
#include <Wire.h>
#include <math.h>

#include "speaker_proto.h"

// ATOM Lite 의 Grove 단자: G26 = SDA, G32 = SCL. 화면 위 LED 는 G27.
#define PIN_SDA 26
#define PIN_SCL 32
#define PIN_LED 27

#define SLEEP_AFTER_MS 30000    // 항상 연결이 꺼져 있으면 마지막 소리 뒤 이만큼 지나 쉰다
#define FIND_MS 60000           // 찾기는 이만큼 하고 끝낸다 (그동안 목록에서 고른다)
#define PENDING_TIMEOUT_MS 25000 // 이 안에 연결이 안 되면 기다리던 소리는 버린다
#define SETTLE_MS 800           // 연결 직후 스피커가 소리를 받을 준비가 될 때까지

enum Mode : uint8_t { MODE_OFF, MODE_CONNECT, MODE_FIND };

// 다시 시작해도 남는 값 (전원을 새로 넣으면 지워진다)
#define RTC_MAGIC 0x5350
RTC_NOINIT_ATTR static uint32_t rtc_magic;
RTC_NOINIT_ATTR static uint8_t rtc_mode;
RTC_NOINIT_ATTR static uint8_t rtc_pending;

static BluetoothA2DPSource a2dp;
static Preferences prefs;

static Mode mode = MODE_OFF;
static bool keep = false;
static uint8_t volume = 60;
static bool has_target = false;
static uint8_t target_addr[6];
static char target_name[SPK_NAME_LEN] = "";

// 찾은 스피커 (블루투스 작업에서 채우고 I2C 작업에서 읽는다)
struct Found {
  bool used;
  uint8_t addr[6];
  int8_t rssi;
  char name[SPK_NAME_LEN];
};
static Found found[SPK_MAX_FOUND];
static portMUX_TYPE found_mux = portMUX_INITIALIZER_UNLOCKED;

// I2C 로 받은 명령 (loop 에서 처리)
static volatile uint8_t cmd_play = 0;
static volatile bool cmd_find = false;
static volatile int cmd_connect = -1;
static volatile bool cmd_forget = false;
static volatile int cmd_keep = -1;
static volatile int cmd_volume = -1;

static uint8_t pending_sound = 0;
static uint32_t pending_since = 0;
static uint32_t last_activity = 0;
static uint32_t mode_since = 0;
static uint32_t connected_since = 0;

// ---- 소리 만들기 ----

#define SAMPLE_RATE 44100
struct Note {
  uint16_t hz;  // 0 이면 쉼
  uint16_t ms;
};
struct Sound {
  const Note *notes;
  uint8_t count;
};

static const Note mic_on_notes[] = {{0, 150}, {660, 80}, {990, 130}};
static const Note mic_off_notes[] = {{0, 150}, {990, 80}, {660, 130}};
static const Note warn_notes[] = {{0, 150}, {880, 150}, {0, 80}, {880, 150}, {0, 80}, {880, 260}};
static const Note reset_notes[] = {{0, 150}, {523, 120}, {659, 120}, {784, 220}};
static const Note test_notes[] = {{0, 200}, {523, 160}, {659, 160}, {784, 160}, {1047, 320}};

static const Sound sounds[] = {
    {nullptr, 0},
    {mic_on_notes, 3},
    {mic_off_notes, 3},
    {warn_notes, 6},
    {reset_notes, 4},
    {test_notes, 5},
};
#define SOUND_COUNT (sizeof(sounds) / sizeof(sounds[0]))

static int16_t sine[256];

// 블루투스 작업과 loop 가 같이 만지는 재생 상태
static portMUX_TYPE play_mux = portMUX_INITIALIZER_UNLOCKED;
static const Sound *playing = nullptr;
static uint8_t note_index = 0;
static uint32_t note_pos = 0;  // 지금 음에서 몇 번째 샘플인지
static uint32_t phase = 0;     // 위상 (위 8비트가 sine 표 번호)
static uint32_t step = 0;      // 샘플마다 위상이 나아가는 양 (음 높이)
static volatile bool is_playing = false;

static void start_sound(uint8_t id) {
  if (id == 0 || id >= SOUND_COUNT) return;
  portENTER_CRITICAL(&play_mux);
  playing = &sounds[id];
  note_index = 0;
  note_pos = 0;
  phase = 0;
  is_playing = true;
  portEXIT_CRITICAL(&play_mux);
  Serial.printf("[spk] play %u\n", id);
}

static int32_t get_frames(Frame *frames, int32_t count) {
  // 소리 크기는 귀에 고르게 들리도록 제곱으로 (최대 약 40%: 스피커가 찢어지지 않게)
  int32_t amp = (int32_t)volume * volume * 13000 / 10000;
  portENTER_CRITICAL(&play_mux);
  for (int32_t i = 0; i < count; i++) {
    int16_t v = 0;
    if (playing) {
      const Note &n = playing->notes[note_index];
      uint32_t len = (uint32_t)n.ms * SAMPLE_RATE / 1000;
      if (n.hz) {
        // 시작 4ms, 끝 20ms 는 부드럽게 (딸깍 소리 방지)
        uint32_t attack = SAMPLE_RATE * 4 / 1000;
        uint32_t release = SAMPLE_RATE * 20 / 1000;
        int32_t env = 256;
        if (note_pos < attack) env = note_pos * 256 / attack;
        if (note_pos + release > len) env = (len - note_pos) * 256 / release;
        v = (int16_t)((int32_t)sine[phase >> 24] * amp / 32767 * env / 256);
        if (note_pos == 0) step = (uint32_t)((uint64_t)n.hz * 4294967296ULL / SAMPLE_RATE);
        phase += step;
      }
      if (++note_pos >= len) {
        note_pos = 0;
        phase = 0;
        if (++note_index >= playing->count) {
          playing = nullptr;
          is_playing = false;
        }
      }
    }
    frames[i].channel1 = v;
    frames[i].channel2 = v;
  }
  portEXIT_CRITICAL(&play_mux);
  return count;
}

// ---- 찾기 ----

// 블루투스가 소리를 받을 수 있는 기기(스피커, 헤드폰)를 찾을 때마다 부른다. false 를 돌려주면 연결하지 않는다.
static bool on_found(const char *name, esp_bd_addr_t addr, int rssi) {
  if (!name || !name[0]) return false;
  portENTER_CRITICAL(&found_mux);
  int slot = -1;
  for (int i = 0; i < SPK_MAX_FOUND; i++) {
    if (found[i].used && memcmp(found[i].addr, addr, 6) == 0) {
      slot = i;
      break;
    }
    if (!found[i].used && slot < 0) slot = i;
  }
  if (slot >= 0) {
    Found &f = found[slot];
    f.used = true;
    memcpy(f.addr, addr, 6);
    f.rssi = rssi < -128 ? -128 : rssi > 0 ? 0 : rssi;
    strlcpy(f.name, name, sizeof(f.name));
  }
  portEXIT_CRITICAL(&found_mux);
  return false;
}

// ---- 상태 ----

static uint8_t state() {
  switch (mode) {
    case MODE_FIND:
      return SPK_STATE_FINDING;
    case MODE_CONNECT:
      if (!a2dp.is_connected()) return SPK_STATE_CONNECTING;
      return is_playing ? SPK_STATE_PLAYING : SPK_STATE_CONNECTED;
    default:
      return SPK_STATE_OFF;
  }
}

static void set_led() {
  static uint8_t last = 0xFF;
  uint8_t s = state();
  if (s == last) return;
  last = s;
  switch (s) {
    case SPK_STATE_CONNECTING:
      rgbLedWrite(PIN_LED, 0, 0, 12);  // 파랑
      break;
    case SPK_STATE_FINDING:
      rgbLedWrite(PIN_LED, 10, 6, 0);  // 노랑
      break;
    case SPK_STATE_CONNECTED:
    case SPK_STATE_PLAYING:
      rgbLedWrite(PIN_LED, 0, 10, 0);  // 초록
      break;
    default:
      rgbLedWrite(PIN_LED, 0, 0, 0);  // 쉬는 중에는 끈다
      break;
  }
}

// ---- I2C ----

static void reply(uint8_t select) {
  uint8_t buf[SPK_REPLY_LEN] = {0};
  if (select == 0xFF) {
    buf[0] = 'A';
    buf[1] = SPK_PROTO;
    buf[2] = state();
    uint8_t n = 0;
    portENTER_CRITICAL(&found_mux);
    for (int i = 0; i < SPK_MAX_FOUND; i++) n += found[i].used;
    portEXIT_CRITICAL(&found_mux);
    buf[3] = n;
    buf[4] = keep;
    buf[5] = volume;
    buf[6] = has_target;
    memcpy(buf + 8, target_name, SPK_NAME_LEN - 1);
  } else {
    buf[0] = 'E';
    if (select < SPK_MAX_FOUND) {
      portENTER_CRITICAL(&found_mux);
      Found f = found[select];
      portEXIT_CRITICAL(&found_mux);
      buf[1] = f.used;
      buf[2] = (uint8_t)f.rssi;
      if (f.used) memcpy(buf + 4, f.name, SPK_NAME_LEN - 1);
    }
  }
  // 원조 ESP32 는 읽기 요청이 온 뒤에 답을 만들 틈이 없어서, 미리 넣어 둔다
  Wire.slaveWrite(buf, sizeof(buf));
}

static void on_receive(int len) {
  uint8_t data[8];
  int n = 0;
  while (Wire.available()) {
    int c = Wire.read();
    if (n < (int)sizeof(data)) data[n++] = c;
  }
  if (n < 1) return;
  switch (data[0]) {
    case SPK_CMD_PLAY:
      if (n >= 2) cmd_play = data[1];
      break;
    case SPK_CMD_FIND:
      cmd_find = true;
      break;
    case SPK_CMD_CONNECT:
      if (n >= 2) cmd_connect = data[1];
      break;
    case SPK_CMD_FORGET:
      cmd_forget = true;
      break;
    case SPK_CMD_CONFIG:
      if (n >= 3) {
        cmd_keep = data[1] ? 1 : 0;
        cmd_volume = data[2] > 100 ? 100 : data[2];
      }
      break;
    case SPK_CMD_SELECT:
      reply(n >= 2 ? data[1] : 0xFF);
      return;
  }
  reply(0xFF);
}

// ---- 상태 바꾸기 ----

// 블루투스를 깨끗하게 다시 켜려고 모듈을 다시 시작한다. 기다리던 소리는 넘겨준다.
static void go(Mode next, uint8_t sound = 0) {
  Serial.printf("[spk] mode %d -> %d\n", mode, next);
  rtc_magic = RTC_MAGIC;
  rtc_mode = next;
  rtc_pending = sound;
  Serial.flush();
  delay(30);
  ESP.restart();
}

static void load_prefs() {
  prefs.begin("spk");
  keep = prefs.getBool("keep", false);
  volume = prefs.getUChar("vol", 60);
  has_target = prefs.getBytes("addr", target_addr, 6) == 6;
  prefs.getString("name", target_name, sizeof(target_name));
}

void setup() {
  Serial.begin(115200);
  load_prefs();
  for (int i = 0; i < 256; i++) sine[i] = (int16_t)(sinf(i * 2 * PI / 256) * 32767);

  Mode boot = (keep && has_target) ? MODE_CONNECT : MODE_OFF;
  if (rtc_magic == RTC_MAGIC && rtc_mode <= MODE_FIND) {
    boot = (Mode)rtc_mode;
    pending_sound = rtc_pending;
  }
  rtc_magic = 0;
  if (boot == MODE_CONNECT && !has_target) boot = MODE_OFF;
  if (boot == MODE_OFF) pending_sound = 0;
  mode = boot;
  mode_since = last_activity = pending_since = millis();

  // 쉬는 중에는 느리게 돌아 전기를 아낀다 (블루투스는 80MHz 이상이 필요)
  if (mode == MODE_OFF) setCpuFrequencyMhz(80);

  Wire.onReceive(on_receive);
  Wire.begin((uint8_t)SPK_ADDR, PIN_SDA, PIN_SCL, 0);
  reply(0xFF);

  if (mode == MODE_CONNECT) {
    // 기억한 주소로 바로 연결 (찾기를 건너뛰어 빠르다). 안 되면 10초마다 다시 시도한다.
    a2dp.set_auto_reconnect(target_addr);
    a2dp.set_ssid_callback(on_found);
    a2dp.set_local_name("LLM Deck Speaker");
    a2dp.start(get_frames);
  } else if (mode == MODE_FIND) {
    a2dp.set_auto_reconnect(false);
    a2dp.set_ssid_callback(on_found);
    a2dp.set_local_name("LLM Deck Speaker");
    a2dp.start(get_frames);
  }
  Serial.printf("[spk] boot mode=%d keep=%d vol=%u target=%s pending=%u\n", mode, keep, volume,
                has_target ? target_name : "-", pending_sound);
  set_led();
}

void loop() {
  uint32_t now = millis();

  if (cmd_keep >= 0) {
    bool k = cmd_keep;
    uint8_t v = cmd_volume;
    cmd_keep = -1;
    if (k != keep) prefs.putBool("keep", k);
    if (v != volume) prefs.putUChar("vol", v);
    bool turned_on = k && !keep;
    keep = k;
    volume = v;
    if (turned_on && mode == MODE_OFF && has_target) go(MODE_CONNECT);
    last_activity = now;
  }

  if (cmd_forget) {
    cmd_forget = false;
    prefs.remove("addr");
    prefs.remove("name");
    has_target = false;
    target_name[0] = 0;
    Serial.println("[spk] forget");
    go(MODE_OFF);
  }

  if (cmd_find) {
    cmd_find = false;
    if (mode != MODE_FIND) go(MODE_FIND);
    mode_since = now;  // 찾는 중에 또 누르면 시간을 늘린다
  }

  if (cmd_connect >= 0) {
    int i = cmd_connect;
    cmd_connect = -1;
    portENTER_CRITICAL(&found_mux);
    Found f = i < SPK_MAX_FOUND ? found[i] : Found{};
    portEXIT_CRITICAL(&found_mux);
    if (f.used) {
      prefs.putBytes("addr", f.addr, 6);
      prefs.putString("name", f.name);
      Serial.printf("[spk] remember %s\n", f.name);
      // 고르자마자 시험 소리를 내서 제대로 연결됐는지 귀로 알 수 있게 한다
      go(MODE_CONNECT, SPK_SOUND_TEST);
    }
  }

  if (cmd_play) {
    uint8_t id = cmd_play;
    cmd_play = 0;
    last_activity = now;
    if (mode == MODE_OFF) {
      if (has_target) go(MODE_CONNECT, id);
    } else if (mode == MODE_CONNECT) {
      pending_sound = id;
      pending_since = now;
    }
  }

  if (mode == MODE_CONNECT) {
    if (a2dp.is_connected()) {
      if (!connected_since) {
        connected_since = now;
        last_activity = now;
        Serial.println("[spk] connected");
      }
      if (pending_sound && now - connected_since >= SETTLE_MS) {
        start_sound(pending_sound);
        pending_sound = 0;
        last_activity = now;
      }
    } else {
      if (connected_since) Serial.println("[spk] disconnected");
      connected_since = 0;
      if (pending_sound && now - pending_since > PENDING_TIMEOUT_MS) {
        Serial.println("[spk] speaker not reachable, drop sound");
        pending_sound = 0;
      }
    }
    if (is_playing) last_activity = now;
    if (!keep && !pending_sound && now - last_activity > SLEEP_AFTER_MS) go(MODE_OFF);
  }

  if (mode == MODE_FIND && now - mode_since > FIND_MS) {
    go(keep && has_target ? MODE_CONNECT : MODE_OFF);
  }

  set_led();
  delay(20);
}
