#!/bin/bash
# 공유 잠금 + 프로토콜 하향 차단 + 실제 기기의 빌드 SHA 검증.
# 다른 worktree 로 전환할 때만 내용을 확인한 뒤 --allow-worktree-switch 를 지정한다.
set -eu
cd "$(dirname "$0")"
PIO_PY="${PIO_PY:-$HOME/.local/share/uv/tools/platformio/bin/python}"
exec "$PIO_PY" scripts/flash_guard.py "$@"
