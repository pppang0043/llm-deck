#include "mic.h"

#include <ESP_I2S.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/gpio.h>

#include "ble_link.h"
#include "link.h"
#include "net.h"
#include "pins.h"
#include "usb_link.h"

static I2SClass i2s;
static WiFiUDP udp;

static volatile bool on = false;
static volatile bool ask = false;
static volatile bool present = false;
static volatile uint8_t level = 0;
static volatile uint32_t target = 0;

// Wi-Fi(UDP) 패킷: [순번 4바이트][PCM 16bit little-endian 320샘플]
struct __attribute__((packed)) Packet {
  uint32_t seq;
  int16_t pcm[MIC_FRAME_SAMPLES];
};

// 마이크 장치(I2S)는 켤 때만 연다. 열어 두면 받는 칸(DMA)이 내부 RAM 을 8KB 남짓 차지한다.
// 열고 닫기는 이 작업 안에서만 해서, 읽는 도중에 닫히는 일이 없게 한다.
static bool open_i2s() {
  i2s.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  if (!i2s.begin(I2S_MODE_STD, MIC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("[mic] i2s begin failed");
    return false;
  }
  return true;
}

// 한 묶음(20ms) 안에서 값이 움직였는지. 진짜 마이크는 조용해도 잡음으로 몇 칸씩 움직인다
// INMP441 은 32비트 칸에 24비트 값을 담아 보낸다. 16비트로 줄이면서 소리를 키운다.
#define MIC_RAW_SHIFT 15
#define MIC_FLAT_SPAN 4
// 이만큼 이어서 값이 그대로면 마이크가 빠졌다고 본다 (1초)
#define MIC_FLAT_FRAMES 50

static void set_present(bool value) {
  if (value == present) return;
  present = value;
  Serial.printf("[mic] device mic %s\n", value ? "found" : "missing");
}

// 켤 때 한 번: 마이크를 0.6초 열어 보고 닫는다 (INMP441 은 켜고 수십 ms 뒤부터 소리를 낸다)
static void probe(int32_t *raw) {
  if (!open_i2s()) return;
  bool moved = false;
  for (int i = 0; i < 30 && !moved; i++) {
    if (i < 10) {
      i2s.readBytes((char *)raw, MIC_FRAME_SAMPLES * 4);
      continue;
    }
    if (i2s.readBytes((char *)raw, MIC_FRAME_SAMPLES * 4) != MIC_FRAME_SAMPLES * 4) continue;
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (int j = 0; j < MIC_FRAME_SAMPLES; j++) {
      int32_t s = raw[j] >> MIC_RAW_SHIFT;
      lo = min(lo, s);
      hi = max(hi, s);
    }
    moved = hi - lo > MIC_FLAT_SPAN;
  }
  i2s.end();
  set_present(moved);
  if (!moved) Serial.println("[mic] device mic missing");
}

// --- 선 진단 ---
static volatile bool diag_want = false, diag_done = false;
static char diag_text[300];

static bool diag_open(i2s_std_slot_mask_t slot) {
  i2s.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  if (!i2s.begin(I2S_MODE_STD, MIC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, slot)) return false;
  vTaskDelay(pdMS_TO_TICKS(500));  // 마이크는 클럭을 받고 수백 ms 뒤부터 소리를 낸다
  return true;
}

static void diag_read(int32_t *raw, int32_t &lo, int32_t &hi) {
  for (int i = 0; i < 5; i++) i2s.readBytes((char *)raw, MIC_FRAME_SAMPLES * 4);
  lo = INT32_MAX;
  hi = INT32_MIN;
  if (i2s.readBytes((char *)raw, MIC_FRAME_SAMPLES * 4) != MIC_FRAME_SAMPLES * 4) return;
  for (int i = 0; i < MIC_FRAME_SAMPLES; i++) {
    int32_t s = raw[i] >> MIC_RAW_SHIFT;
    lo = min(lo, s);
    hi = max(hi, s);
  }
}

// 보드가 내보내는 클럭이 핀에서 실제로 오르내리는지. 어딘가에 붙어 있으면 멈춰 있다
static const char *diag_clock(int pin) {
  gpio_input_enable((gpio_num_t)pin);
  int edges = 0, highs = 0, last = gpio_get_level((gpio_num_t)pin);
  for (int i = 0; i < 20000; i++) {
    int level = gpio_get_level((gpio_num_t)pin);
    edges += level != last;
    highs += level;
    last = level;
  }
  return edges > 20 ? "정상" : highs > 10000 ? "3V3에 붙어 있음" : "GND에 붙어 있음";
}

static void diag_run(int32_t *raw) {
  if (!diag_open(I2S_STD_SLOT_LEFT)) {
    snprintf(diag_text, sizeof(diag_text), "I2S를 열지 못했습니다");
    return;
  }
  int32_t lo, hi, up_lo, up_hi, down_lo, down_hi;
  diag_read(raw, lo, hi);
  bool left = hi - lo > MIC_FLAT_SPAN;
  const char *sck = diag_clock(MIC_SCK), *ws = diag_clock(MIC_WS);
  // 아무도 SD 를 구동하지 않으면 값이 보드 안의 저항이 끄는 쪽을 따라간다
  gpio_pullup_en((gpio_num_t)MIC_SD);
  diag_read(raw, up_lo, up_hi);
  gpio_pullup_dis((gpio_num_t)MIC_SD);
  gpio_pulldown_en((gpio_num_t)MIC_SD);
  diag_read(raw, down_lo, down_hi);
  gpio_pulldown_dis((gpio_num_t)MIC_SD);
  i2s.end();
  bool right = false;
  if (!left && diag_open(I2S_STD_SLOT_RIGHT)) {
    diag_read(raw, lo, hi);
    right = hi - lo > MIC_FLAT_SPAN;
    i2s.end();
  }

  const char *sd;
  if (left) {
    sd = "정상 (소리 값이 들어옴)";
  } else if (right) {
    sd = "오른쪽 채널로만 들어옴\nL/R과 GND 연결을 확인하세요";
  } else if (up_hi < 0 && down_lo >= 0) {
    sd = "보내는 쪽 없음\nSD, VDD, GND, SCK, WS 선이나\n마이크 칩을 확인하세요";
  } else if (up_hi >= 0 && down_lo >= 0 && up_hi == down_hi) {
    sd = "GND에 붙어 있음";
  } else if (up_hi < 0 && down_lo < 0) {
    sd = "3V3에 붙어 있음";
  } else {
    sd = "값이 불안정함\n접촉 불량을 확인하세요";
  }
  snprintf(diag_text, sizeof(diag_text), "SCK: %s\nWS: %s\nSD: %s", sck, ws, sd);
  set_present(left);
  Serial.printf("[mic] diag sck=%d ws=%d left=%d right=%d up=%d..%d down=%d..%d\n", sck[0] != 'G' && sck[0] != '3',
                ws[0] != 'G' && ws[0] != '3', left, right, (int)up_lo, (int)up_hi, (int)down_lo, (int)down_hi);
}

static void mic_task(void *) {
  static int32_t raw[MIC_FRAME_SAMPLES];
  static Packet packet;
  static uint8_t trailer[NET_UDP_TRAILER];
  uint32_t seq = 0;
  bool opened = false;
  int flat = 0;  // 이어서 값이 그대로였던 묶음 수

  probe(raw);
  for (;;) {
    if (diag_want) {
      if (opened || on || ask) {
        snprintf(diag_text, sizeof(diag_text), "마이크를 쓰는 중입니다\n끝난 뒤 다시 눌러 주세요");
      } else {
        diag_run(raw);
      }
      diag_want = false;
      diag_done = true;
    }
    bool want = on || ask;
    if (want != opened) {
      if (want) {
        opened = open_i2s();
        if (!opened) {
          vTaskDelay(pdMS_TO_TICKS(1000));
          continue;
        }
      } else {
        i2s.end();
        opened = false;
        level = 0;
        flat = 0;
      }
    }
    if (!opened) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    size_t got = i2s.readBytes((char *)raw, sizeof(raw));
    if (got != sizeof(raw) || !(on || ask)) {
      level = 0;
      if (got == 0) vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    int32_t peak = 0, lo = INT32_MAX, hi = INT32_MIN;
    for (int i = 0; i < MIC_FRAME_SAMPLES; i++) {
      int32_t s = constrain(raw[i] >> MIC_RAW_SHIFT, -32768, 32767);
      packet.pcm[i] = s;
      if (abs(s) > peak) peak = abs(s);
      lo = min(lo, s);
      hi = max(hi, s);
    }
    level = peak * 100 / 32768;
    if (hi - lo > MIC_FLAT_SPAN) {
      flat = 0;
      set_present(true);
    } else if (++flat >= MIC_FLAT_FRAMES) {
      set_present(false);
    }

    packet.seq = seq++;
    switch (link_active()) {
      case LINK_USB:
        usb_send_audio(packet.seq, packet.pcm, MIC_FRAME_SAMPLES);
        break;
      case LINK_BLE:
        ble_send_audio(packet.seq, packet.pcm, MIC_FRAME_SAMPLES);
        break;
      case LINK_WIFI:
        // 서명을 붙인다 (net.h)
        if (target != 0 && net_udp_trailer((const uint8_t *)&packet, sizeof(packet), trailer)) {
          udp.beginPacket(IPAddress(target), MIC_UDP_PORT);
          udp.write((const uint8_t *)&packet, sizeof(packet));
          udp.write(trailer, sizeof(trailer));
          udp.endPacket();
        }
        break;
      default:
        break;
    }
  }
}

void mic_init() {
  xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 5, NULL, 0);
}

void mic_set_on(bool value) {
  on = value;
  Serial.printf("[mic] %s\n", value ? "on" : "off");
}

bool mic_is_on() {
  return on;
}

void mic_set_ask(bool value) {
  ask = value;
}

bool mic_present() {
  return present;
}

void mic_diag_start() {
  diag_done = false;
  diag_want = true;
}

const char *mic_diag_result() {
  return diag_done ? diag_text : nullptr;
}

uint8_t mic_level() {
  return level;
}

void mic_set_target(IPAddress ip) {
  target = (uint32_t)ip;
}

IPAddress mic_target() {
  return IPAddress(target);
}
