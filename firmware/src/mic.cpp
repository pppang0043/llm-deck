#include "mic.h"

#include <ESP_I2S.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "ble_link.h"
#include "link.h"
#include "pins.h"
#include "usb_link.h"

static I2SClass i2s;
static WiFiUDP udp;

static volatile bool on = false;
static volatile uint8_t level = 0;
static volatile uint32_t target = 0;

// Wi-Fi(UDP) 패킷: [순번 4바이트][PCM 16bit little-endian 320샘플]
struct __attribute__((packed)) Packet {
  uint32_t seq;
  int16_t pcm[MIC_FRAME_SAMPLES];
};

static void mic_task(void *) {
  static int32_t raw[MIC_FRAME_SAMPLES];
  static Packet packet;
  uint32_t seq = 0;

  for (;;) {
    size_t got = i2s.readBytes((char *)raw, sizeof(raw));
    if (got != sizeof(raw) || !on) {
      level = 0;
      if (got == 0) vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // INMP441 은 32비트 칸에 24비트 값을 담아 보낸다. 16비트로 줄이면서 소리를 키운다.
    int32_t peak = 0;
    for (int i = 0; i < MIC_FRAME_SAMPLES; i++) {
      int32_t s = constrain(raw[i] >> 14, -32768, 32767);
      packet.pcm[i] = s;
      if (abs(s) > peak) peak = abs(s);
    }
    level = peak * 100 / 32768;

    packet.seq = seq++;
    switch (link_active()) {
      case LINK_USB:
        usb_send_audio(packet.seq, packet.pcm, MIC_FRAME_SAMPLES);
        break;
      case LINK_BLE:
        ble_send_audio(packet.seq, packet.pcm, MIC_FRAME_SAMPLES);
        break;
      case LINK_WIFI:
        if (target != 0) {
          udp.beginPacket(IPAddress(target), MIC_UDP_PORT);
          udp.write((const uint8_t *)&packet, sizeof(packet));
          udp.endPacket();
        }
        break;
      default:
        break;
    }
  }
}

void mic_init() {
  i2s.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  if (!i2s.begin(I2S_MODE_STD, MIC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("[mic] i2s begin failed");
    return;
  }
  xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 5, NULL, 0);
}

void mic_set_on(bool value) {
  on = value;
  Serial.printf("[mic] %s\n", value ? "on" : "off");
}

bool mic_is_on() {
  return on;
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
