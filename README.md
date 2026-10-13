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

## 사용량 페이지

| 페이지 | 보여주는 것 | 준비할 것 |
|---|---|---|
| Claude Code | 구독 한도 사용률 (5시간 / 주간) | PC 마다 한 번 `uv run main.py --install-statusline` |
| Codex | ChatGPT 구독 한도 사용률 (5시간 / 주간) | 없음 (그 PC 에서 Codex CLI 를 쓴 적이 있으면 나온다) |
| Anthropic API / OpenAI API | 비용, 토큰 | `companion/config.example.toml` 을 `config.toml` 로 복사해 Admin 키를 채운다 |

한도 사용률은 계정 기준 값이라 다른 PC 에서 쓴 양도 포함된다. 다만 Claude Code / Codex 가
마지막으로 응답한 시점의 값이며, 그 시각이 페이지 제목 옆에 나온다.

기기는 마지막으로 받은 값을 기억한다. PC 없이 전원만 연결해도 그 값을 보여준다.

### 여러 PC 에서 쓰기

Claude Code / Codex 가 없는 PC 에 연결해도 최신 값을 보려면, 값을 비공개 GitHub gist 로 공유한다.

1. github.com > Settings > Developer settings > Personal access tokens 에서 gist 권한만 있는 토큰을 만든다
2. `companion/config.toml` 의 `[sync] github_token` 에 적고 `uv run main.py --setup-sync` 를 한 번 실행한다
3. 출력된 `gist_id` 를 모든 PC 의 `config.toml` 에 토큰과 함께 적는다

Claude Code / Codex 를 쓰는 PC 에서 도우미가 돌고 있으면 새 값이 올라간다.

기기는 도우미가 한 번 알려준 공유 주소를 기억한다. 그 뒤로는 PC 도우미가 없어도, Wi-Fi 만 연결되어 있으면
기기가 직접 값을 읽어 온다 (화면 왼쪽 위에 "Cloud"). 이때 PC 는 전원만 주면 되고 아무 설정도 필요 없다.
공유 저장소 반영이 최대 5분 늦을 수 있다.

### Wi-Fi 여러 곳 등록

다른 장소의 Wi-Fi 나 휴대폰 핫스팟을 추가로 등록하면, 기기가 그중 잡히는 곳에 알아서 접속한다 (최대 5개, 2.4GHz 만).
기기를 USB 로 연결한 상태에서 실행한다. 펌웨어를 다시 올릴 필요는 없다.

```sh
uv run main.py --add-wifi     # 이름과 비밀번호를 물어본다
uv run main.py --clear-wifi   # 추가로 등록한 것을 모두 지운다
```
기기를 연결하지 않는 PC(예: 항상 켜 둔 미니 PC)에서는 `uv run main.py --sync-only` 로 공유만 한다.

`--install-statusline` 은 Claude Code 설정(`~/.claude/settings.json`)의 상태줄 명령 앞에 값을 저장하는
스크립트를 끼워 넣는다. 원래 상태줄 명령은 그대로 이어서 실행된다. 되돌리려면 `--uninstall-statusline`.

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
