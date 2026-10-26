#include "mic.h"

#include <ESP_I2S.h>
#include <WiFi.h>
#include <WiFiUdp.h>

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
      int32_t s = raw[j] >> 14;
      lo = min(lo, s);
      hi = max(hi, s);
    }
    moved = hi - lo > MIC_FLAT_SPAN;
  }
  i2s.end();
  set_present(moved);
  if (!moved) Serial.println("[mic] device mic missing");
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

    // INMP441 은 32비트 칸에 24비트 값을 담아 보낸다. 16비트로 줄이면서 소리를 키운다.
    int32_t peak = 0, lo = INT32_MAX, hi = INT32_MIN;
    for (int i = 0; i < MIC_FRAME_SAMPLES; i++) {
      int32_t s = constrain(raw[i] >> 14, -32768, 32767);
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

uint8_t mic_level() {
  return level;
}

void mic_set_target(IPAddress ip) {
  target = (uint32_t)ip;
}

IPAddress mic_target() {
  return IPAddress(target);
}
