#!/bin/bash
# 펌웨어를 USB 로 올린다 (Mac). 빌드 → 도우미 끄기 → 올리기 → 도우미 켜기 → 기기가 새 펌웨어로 켜졌는지 확인.
#   ./flash.sh            기억해 둔 기기 (~/.llm-deck/usb_serial)
#   ./flash.sh PORT       포트를 직접 지정 (/dev/cu.usbmodem...)
#
# 올리는 중에 다른 프로그램(도우미, 테스트 스크립트)이 같은 포트를 읽으면 올리기가 중간에 끊기고 기기가 반쯤
# 써진 채 멈춘다. 그래서
#   - 한 번에 하나만 빌드·올리기 (다른 곳에서 올리는 중이면 끝날 때까지 기다린다. 같은 빌드 폴더를 같이 쓰면 ELF 가 깨진다)
#   - ~/.llm-deck/flashing 표시를 만들어 두는 동안 도우미는 포트를 열지 않는다 (transports/usb.py)
#   - 포트를 잡고 있는 프로그램이 남아 있으면 올리지 않고 누구인지 알린다
set -u
cd "$(dirname "$0")"

PIO="${PIO:-$HOME/.local/bin/pio}"
PIO_PY="${PIO_PY:-$HOME/.local/share/uv/tools/platformio/bin/python}"
ENV_NAME=llm-deck
STATE="$HOME/.llm-deck"
LOCK_DIR="$STATE/flash.lock"
FLAG="$STATE/flashing"
LOG="$STATE/companion.log"
AGENT=com.llmdeck.companion
PLIST="$HOME/Library/LaunchAgents/$AGENT.plist"
mkdir -p "$STATE"

# 비밀번호 같은 것이 섞인 줄은 보이지 않는다
quiet() { grep -v -E "auth=|Namespace"; }

# ---- 한 번에 하나만 ----
waited=0
until mkdir "$LOCK_DIR" 2>/dev/null; do
  owner=$(cat "$LOCK_DIR/pid" 2>/dev/null)
  if [ -z "$owner" ] || ! kill -0 "$owner" 2>/dev/null; then
    rm -rf "$LOCK_DIR"  # 죽은 채 남은 잠금
    continue
  fi
  if [ $waited -ge 600 ]; then
    echo "[flash] 다른 곳(pid $owner)에서 10분 넘게 올리는 중입니다. 그쪽이 끝난 뒤 다시 하세요."
    exit 1
  fi
  [ $waited -eq 0 ] && echo "[flash] 다른 곳(pid $owner)에서 빌드·올리는 중이라 기다립니다..."
  sleep 5
  waited=$((waited + 5))
done
echo $$ >"$LOCK_DIR/pid"

helper_stopped=0
finish() {
  rm -f "$FLAG"
  if [ $helper_stopped -eq 1 ] && [ -f "$PLIST" ]; then
    launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null
  fi
  rm -rf "$LOCK_DIR"
}
trap finish EXIT

# ---- 빌드 (도우미를 끄기 전에: 기기가 쉬는 시간을 줄인다) ----
"$PIO" run -e "$ENV_NAME" 2>&1 | quiet | grep -E "error|warning: unused|SUCCESS|FAILED"
if [ "${PIPESTATUS[0]}" -ne 0 ]; then
  echo "[flash] 빌드 실패. 올리지 않았습니다."
  exit 1
fi

find_port() {
  if [ $# -gt 0 ] && [ -n "$1" ]; then
    echo "$1"
    return
  fi
  "$PIO_PY" - "$(cat "$STATE/usb_serial" 2>/dev/null)" <<'EOF'
import sys
from serial.tools import list_ports
known = sys.argv[1]
ports = [p for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001]
ports.sort(key=lambda p: (p.serial_number or "") != known)
print(ports[0].device if ports else "")
EOF
}

# ---- 도우미 끄기 ----
touch "$FLAG"
launchctl bootout "gui/$(id -u)/$AGENT" 2>/dev/null
pkill -f 'LLM Deck.app' 2>/dev/null
helper_stopped=1

PORT=$(find_port "${1:-}")
if [ -z "$PORT" ]; then
  echo "[flash] USB 로 연결된 기기가 없습니다."
  exit 1
fi

# 터미널에서 띄운 도우미 등은 표시를 보고 몇 초 안에 포트를 놓는다 (0.5초마다 확인)
for _ in $(seq 1 20); do
  holders=$(lsof -t "$PORT" 2>/dev/null)
  [ -z "$holders" ] && break
  sleep 0.5
done
if [ -n "$holders" ]; then
  echo "[flash] $PORT 를 다른 프로그램이 잡고 있어 올리지 않았습니다:"
  ps -o pid=,command= -p "$(echo "$holders" | paste -sd, -)" | cut -c1-160
  echo "[flash] 그 프로그램을 끈 뒤 다시 하세요."
  exit 1
fi

# ---- 올리기 (끊기면 한 번 더: 기기가 다운로드 모드에 멈춰 있어도 다시 올리면 살아난다) ----
log_start=$(wc -c <"$LOG" 2>/dev/null || echo 0)
ok=0
for try in 1 2; do
  echo "[flash] $PORT 로 올리는 중 ($try 번째)..."
  "$PIO" run -e "$ENV_NAME" -t upload --upload-port "$PORT" 2>&1 | quiet | grep -E "error|Error|SUCCESS|FAILED"
  if [ "${PIPESTATUS[0]}" -eq 0 ]; then
    ok=1
    break
  fi
  sleep 3
  PORT=$(find_port "${1:-}")  # 기기가 다시 켜지며 포트 이름이 바뀌었을 수 있다
  [ -z "$PORT" ] && break
done
if [ $ok -ne 1 ]; then
  echo "[flash] 올리기 실패. 기기가 멈춰 있으면 BOOT 버튼을 누른 채 리셋한 뒤 다시 하세요."
  exit 1
fi

# ---- 도우미 켜고 새 펌웨어로 켜졌는지 확인 ----
rm -f "$FLAG"
if [ -f "$PLIST" ]; then
  launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null
  helper_stopped=0
  for _ in $(seq 1 30); do
    sleep 1
    line=$(tail -c +"$((log_start + 1))" "$LOG" 2>/dev/null | grep -a "\[version\] 기기 펌웨어" | tail -1)
    if [ -n "$line" ]; then
      echo "[flash] 완료: ${line#*\] }"
      exit 0
    fi
  done
  echo "[flash] 올렸지만 30초 안에 기기가 버전을 알려 오지 않았습니다. 도우미 기록($LOG)을 확인하세요."
  exit 1
fi
echo "[flash] 완료 (도우미는 켜지 않았습니다)"
