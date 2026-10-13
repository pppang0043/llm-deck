# LLM Deck

Waveshare ESP32-S3-Touch-LCD-2 보드로 만든 책상용 기기.
LLM 사용량을 화면에 보여주고, 버튼으로 켜고 끄는 무선 마이크로 동작한다.

- `firmware/` — 보드에 올라가는 프로그램 (PlatformIO + Arduino)
- `companion/` — PC 에서 실행하는 도우미 (Python). 사용량을 모아 기기로 보내고, 기기의 마이크 소리를 받는다

## 처음 설정

보드를 USB-C 로 연결하고 펌웨어를 올린다.

```sh
cd firmware
pio run -t upload        # 빌드 + 업로드
pio device monitor       # 보드가 내보내는 로그 보기 (Ctrl+C 로 종료)
```

PC 도우미가 USB 포트를 쓰는 동안에는 업로드와 로그 보기가 안 된다. 도우미를 먼저 끈다 (Ctrl+C).

## PC 와 연결하기

기기는 세 가지 방식을 모두 켜 두고, 가장 최근에 데이터를 보내온 쪽을 쓴다 (화면 왼쪽 위에 표시).
PC 도우미에서 방식을 고른다.

```sh
cd companion
uv run main.py               # 되는 방식을 자동 선택 (USB -> 블루투스 -> Wi-Fi 순)
uv run main.py --link usb    # USB 케이블
uv run main.py --link ble    # 블루투스
uv run main.py --link wifi   # Wi-Fi
```

| 방식 | 준비할 것 | 마이크 음질 |
|---|---|---|
| USB | 케이블만 연결 | 원본 그대로 |
| 블루투스 | Mac 은 시스템 설정 > 개인정보 보호 및 보안 > Bluetooth 에서 터미널 앱 허용 | 1/4 로 압축 (ADPCM) |
| Wi-Fi | `firmware/include/secrets.h` 에 공유기 Wi-Fi 이름/비밀번호(2.4GHz)를 적고 펌웨어를 다시 올린다. PC 와 같은 공유기여야 한다 | 원본 그대로 |

API 사용량까지 보려면 `companion/config.example.toml` 을 `config.toml` 로 복사해 키를 채운다.

## 사용법

- 화면을 좌우로 밀면 서비스별 페이지가 넘어간다
- 화면 아래 버튼이나 보드의 BOOT 버튼을 누르면 마이크가 켜지고 꺼진다
- PC 에서도 켜고 끌 수 있다: `uv run main.py --mic on`

## 마이크 (INMP441) 연결

| INMP441 | 보드 헤더 |
|---|---|
| VDD | 3V3 |
| GND | GND |
| L/R | GND |
| SCK | GPIO18 |
| WS | TXD (GPIO43) |
| SD | RXD (GPIO44) |

PC 에서 마이크로 쓰려면 가상 오디오 장치가 필요하다 (Mac: [BlackHole](https://existential.audio/blackhole/) 2ch, Windows: VB-CABLE).
도우미가 기기의 소리를 그 장치로 내보내므로, 다른 프로그램에서는 입력 장치로 BlackHole 을 고르면 된다.
가상 장치 없이 수신만 확인하려면 `uv run main.py --mic on --record test.wav` (10초간 녹음해 파일로 저장).
