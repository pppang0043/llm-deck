# LLM Deck

Waveshare ESP32-S3-Touch-LCD-2 보드로 만든 책상용 기기.
LLM 사용량을 화면에 보여주고, 버튼으로 켜고 끄는 무선 마이크로 동작한다.

- `firmware/` — 보드에 올라가는 프로그램 (PlatformIO + Arduino)
- `companion/` — PC 에서 실행하는 도우미 (Python). 사용량을 모아 기기로 보내고, 기기의 마이크 소리를 받는다

## 처음 설정

1. `firmware/include/secrets.example.h` 를 `secrets.h` 로 복사하고 Wi-Fi 이름/비밀번호를 적는다 (2.4GHz 만 가능)
2. 보드를 USB-C 로 연결하고 펌웨어를 올린다

   ```sh
   cd firmware
   pio run -t upload        # 빌드 + 업로드
   pio device monitor       # 보드가 내보내는 로그 보기 (Ctrl+C 로 종료)
   ```

3. PC 도우미를 실행한다 (기기와 같은 Wi-Fi 에 있어야 한다)

   ```sh
   cd companion
   uv run main.py
   ```

API 사용량까지 보려면 `companion/config.example.toml` 을 `config.toml` 로 복사해 키를 채운다.

## 사용법

- 화면을 좌우로 밀면 서비스별 페이지가 넘어간다
- 화면 아래 버튼이나 보드의 BOOT 버튼을 누르면 마이크가 켜지고 꺼진다

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
가상 장치 없이 수신만 확인하려면 `uv run main.py --record test.wav`.
