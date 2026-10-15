# LLM Deck

Waveshare ESP32-S3-Touch-LCD-2 보드로 만든 책상용 기기.
LLM 구독 한도 사용률을 보여주고, 카메라로 사진·동영상을 찍어 PC 에 저장하며, PC 의 마이크 입력으로도 쓴다.

- `firmware/` — 보드에 올라가는 프로그램 (PlatformIO + Arduino)
- `companion/` — PC 에서 실행하는 도우미 (Python, Mac·Windows). 사용량을 모아 기기로 보내고, 기기가 보내는 사진·동영상·소리를 받는다

## 기기 화면

켜면 홈 화면이 나온다. 어느 화면에서든 **오른쪽으로 밀면 홈**으로 돌아온다.

| 아이콘 | 하는 일 |
|---|---|
| 사용량 | Claude Code / Codex 한도 사용률 (5시간·주간). 서비스가 여럿이면 위아래로 넘긴다 |
| 카메라 | 미리보기. 흰 버튼 = 사진(1600x1200), 빨간 버튼 = 녹화(640x480). 아래로 밀면 닫힌다. PC 로 보내는 동안 위에 진행률이 나오고, 블루투스처럼 오래 걸린 전송은 끝나면 알림이 뜬다 |
| 마이크 | 켜고 끄기 (BOOT 버튼도 같다). 켜고 끌 때 PC 에서 알림음 |
| 밝기 | 누를 때마다 100 → 60 → 30% |
| 정보 | 연결 상태, Wi-Fi 주소, 배터리, 메모리, 펌웨어 빌드 시각 |
| 설정 | 아래 표 |

상태줄: 왼쪽은 지금 연결된 방식, 오른쪽은 날짜·시각, 배터리(충전 중이면 번개, 다 차면 100%), 화면 돌리기 버튼.

| 설정 | 내용 |
|---|---|
| 화면 밝기 | 10~100% |
| 배터리 절약 | 배터리로만 쓸 때 만지지 않으면 1분 뒤 어둡게, 3분 뒤 끈다. 깨우는 첫 터치는 버튼을 누르지 않는다 |
| 한도 알림 | 사용률이 80%·90% 를 넘거나 한도가 초기화되면 화면 알림 + PC 알림음 |
| 자동 회전 | 기울이면 화면이 따라 돈다 |
| 마이크 알림음 | 마이크를 켜고 끌 때 PC 에서 소리 |
| 카메라 방향 / 좌우 반전 | 사진이 누워 있거나 뒤집혀 보일 때만 바꾼다 (지금 카메라는 회전 0°, 반전 켬이 맞다) |
| 연결 방식 | USB / 블루투스 / Wi-Fi / 클라우드를 각각 켜고 끈다 |
| 기기 재시작 | 3초 안에 두 번 누른다 |

## PC 와 연결하기

기기는 여러 방식을 동시에 켜 두고, 살아 있는 것 중 **USB > 블루투스 > Wi-Fi > 클라우드** 순으로 하나를 쓴다.
사진·동영상·알림음·마이크 소리는 그 PC 로 간다. 두 PC 가 동시에 붙어도 우선순위가 높은 쪽이 유지된다.

| 방식 | 준비할 것 | 사진 | 동영상 |
|---|---|---|---|
| USB | 케이블 | 0.3초 | 초당 약 16장 |
| 블루투스 | PC 블루투스만 켠다. 운영체제에서 "페어링" 하지 않는다 | 200KB 에 약 9초 | 안 됨 (USB 를 연결하라고 안내) |
| Wi-Fi | 기기와 PC 가 같은 공유기. 기기에 Wi-Fi 등록 (아래) | 1초 안팎 | 초당 약 14장 |
| 클라우드 | 없음. PC 없이 Wi-Fi 만 있으면 기기가 공유된 사용량을 직접 읽는다 | - | - |

블루투스로 붙어 있다가 USB 를 꽂으면 도우미가 30초 안에 USB 로 바꾼다.

## PC 도우미

### Mac

```sh
cd companion
sh macos/install.sh            # 로그인할 때 자동 실행 (블루투스 권한을 받으려고 작은 앱으로 감싼다)
sh macos/install.sh uninstall  # 제거
```

기록: `~/.llm-deck/companion.log`. 자동 실행 중인 도우미를 잠시 끄기:
`launchctl bootout gui/$(id -u)/com.llmdeck.companion && pkill -f 'LLM Deck.app'`

### Windows

`dist/llm-deck-companion-windows.zip` 을 풀고 `windows\install.cmd` 를 더블클릭한다. 자세한 내용은 압축 안의 `README.txt`.
설치는 uv(파이썬 도구) 설치, 방화벽 허용(같은 공유기 안에서만), Claude Code 상태줄 연결, 자동 실행 등록을 한다.
기록: `C:\Users\<이름>\.llm-deck\companion.log`.

압축 파일 다시 만들기 (gist 주소만 넣고 토큰은 넣지 않는다): 이 저장소에서 `companion/.venv/bin/python companion/windows/make_zip.py`.

### 저장 위치

| | Mac | Windows |
|---|---|---|
| 사진 | `~/Pictures/LLM Deck` | `%USERPROFILE%\Pictures\LLM Deck` |
| 동영상 (MOV, Motion JPEG 원본 그대로) | `~/Movies/LLM Deck` | `%USERPROFILE%\Videos\LLM Deck` |

폴더에 쓸 수 없으면(Windows 의 "제어된 폴더 액세스" 등) `~/.llm-deck/photos`, `~/.llm-deck/videos` 에 저장한다.
`config.toml` 의 `[photos] dir`, `[videos] dir` 로 바꿀 수 있다. 기기에 microSD 카드를 넣으면 사진은 카드의 `/photos` 에도 저장된다.

### 명령

도우미는 PC 에 하나만 돈다. 자동 실행 중이면 직접 실행하기 전에 끈다 (두 번째로 켜면 안내만 하고 끝난다).

```sh
uv run main.py                     # 되는 방식으로 자동 연결 (USB -> 블루투스 -> Wi-Fi)
uv run main.py --link ble          # 방식 고르기: usb / ble / wifi
uv run main.py --install-statusline  # Claude Code 한도 사용률 읽기 (PC 마다 한 번)
uv run main.py --add-wifi          # 기기에 Wi-Fi 추가 (USB 로 연결한 상태, 최대 5개, 2.4GHz)
uv run main.py --setup-sync        # 여러 PC 가 사용률을 공유할 비공개 gist 만들기 (처음 한 번)
uv run main.py --test-sound        # 알림음 들어 보기
uv run main.py --camera shoot      # 사진 찍기 (open / record / close / dump)
uv run main.py --test-video 5      # 5초 녹화
uv run main.py --screenshot        # 기기 화면을 찍어 사진 폴더에 저장 (--view home|usage|info|settings 와 함께)
uv run main.py --links ble=off     # 연결 방식 켜고 끄기 (USB 로만, USB 를 꺼 둬도 받는다)
uv run main.py --camera-mount 0m   # 카메라 방향 (0~3, 좌우 반전이면 m)
uv run main.py --battery 4.12      # 멀티미터로 잰 배터리 전압으로 보정 (0 이면 보정 지움)
```

## 여러 PC 에서 사용률 함께 보기

Claude Code / Codex 가 없는 PC 에 연결해도 최신 값을 보려면, 값을 비공개 GitHub gist 로 공유한다.
값을 올리는 PC 는 `gh auth login -s gist` 로 로그인하거나 `config.toml` 의 `[sync] github_token` 에 gist 권한 토큰을 적는다.
받기만 하는 PC 는 `gist_id` 만 있으면 된다. 기기에 연결된 PC 에 값이 있으면 그 값을 먼저 쓰고, 없을 때만 공유된 값을 쓴다.
기기는 공유 주소를 기억해, PC 가 없어도 Wi-Fi 로 직접 읽는다 (상태줄 "클라우드").

## 펌웨어 올리기

```sh
cd firmware
~/.local/bin/pio run -t upload
```

도우미가 USB 포트를 잡고 있으면 올라가지 않는다. 먼저 끈다.
`firmware/include/secrets.h` 에 기본 Wi-Fi 이름·비밀번호를 적는다 (git 에 올라가지 않는다).

## 알아둘 것

- 보드에는 마이크가 없다. INMP441 I2S 마이크를 달면(SCK 18, WS 43, SD 44) 마이크 소리가 PC 로 간다. PC 에서 마이크로 쓰려면 Mac 은 BlackHole, Windows 는 VB-CABLE 이 필요하다
- 블루투스 연결은 기기가 한 번에 하나만 받는다. 다른 PC 가 블루투스로 붙어 있으면 보이지 않는다
- 기기 내부 RAM 이 빠듯하다. 카메라용 20KB 를 부팅 때 미리 잡아 두고, 카메라를 안 쓸 때 클라우드(HTTPS) 받아 오기에 잠깐 빌려준다
- 연결이 바뀐 직후 75초 동안은 앞 연결을 살아 있다고 본다. 같은 PC 에서 USB 로 쓰다가 Wi-Fi 로만 바꾸면 그동안 사진이 USB 쪽으로 간다
- 배터리 % 는 전압으로 짐작한다. USB 에 꽂혀 충전이 끝난 채 20분이 지나면 스스로 보정한다
