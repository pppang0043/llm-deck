"""Claude Code 상태줄에 끼워 넣어 한도 사용률을 파일로 남기는 스크립트를 설치/제거한다.

    uv run main.py --install-statusline
    uv run main.py --uninstall-statusline

원래 있던 상태줄 명령은 그대로 이어서 실행되므로 기존 상태줄 표시는 바뀌지 않는다.
"""

import json
import os
import shutil
from pathlib import Path

DATA_DIR = Path.home() / ".llm-deck"
SNAPSHOT = DATA_DIR / "claude-statusline.json"
WINDOWS = os.name == "nt"
SCRIPT = DATA_DIR / ("statusline.ps1" if WINDOWS else "statusline.sh")
# 원래 있던 상태줄 명령. Windows 에서는 이것을 그대로 실행할 배치 파일(NEXT_CMD)도 함께 만든다.
NEXT = DATA_DIR / "statusline-next"
NEXT_CMD = DATA_DIR / "statusline-next.cmd"
SETTINGS = Path(os.environ.get("CLAUDE_CONFIG_DIR", Path.home() / ".claude")) / "settings.json"
BACKUP = SETTINGS.with_name("settings.json.llm-deck-backup")

SCRIPT_BODY = """#!/bin/sh
# LLM Deck: Claude Code 가 넘겨준 사용 정보를 파일로 남기고, 원래 상태줄 명령을 이어서 실행한다.
dir="$HOME/.llm-deck"
payload=$(cat)
case "$payload" in
  *'"rate_limits"'*)
    printf '%s' "$payload" >"$dir/claude-statusline.tmp.$$" 2>/dev/null &&
      mv -f "$dir/claude-statusline.tmp.$$" "$dir/claude-statusline.json" 2>/dev/null
    ;;
esac
if [ -r "$dir/statusline-next" ]; then
  printf '%s' "$payload" | sh "$dir/statusline-next"
fi
"""

# Windows 용 (PowerShell). 하는 일은 위와 같다.
SCRIPT_BODY_WINDOWS = """# LLM Deck: Claude Code 가 넘겨준 사용 정보를 파일로 남기고, 원래 상태줄 명령을 이어서 실행한다.
$ErrorActionPreference = 'SilentlyContinue'
$utf8 = New-Object System.Text.UTF8Encoding $false
[Console]::InputEncoding = $utf8
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8
$dir = Join-Path $env:USERPROFILE '.llm-deck'
$payload = [Console]::In.ReadToEnd()
if ($payload -match '"rate_limits"') {
  # 임시 파일을 거치지 않고 바로 쓴다. Claude Code 가 실행 중인 스크립트를 중단시키면 임시 파일이 남기 때문이다.
  # 쓰다 만 파일은 도우미가 건너뛴다.
  [IO.File]::WriteAllText((Join-Path $dir 'claude-statusline.json'), $payload, $utf8)
}
$next = Join-Path $dir 'statusline-next.cmd'
if (Test-Path $next) {
  $payload | & $next
}
"""


def _command() -> str:
    if WINDOWS:
        shell = "pwsh" if shutil.which("pwsh") else "powershell"
        return f'{shell} -NoProfile -ExecutionPolicy Bypass -File "{SCRIPT}"'
    return f'sh "{SCRIPT}"'


def _write_next(command: str) -> None:
    NEXT.write_text(command + "\n", encoding="utf-8")
    if WINDOWS:
        # 배치 파일에서 % 는 특수 문자라 두 번 적어야 그대로 전달된다
        NEXT_CMD.write_text("@echo off\r\n" + command.replace("%", "%%") + "\r\n", encoding="utf-8")


def _load() -> dict:
    return json.loads(SETTINGS.read_text(encoding="utf-8")) if SETTINGS.exists() else {}


def _save(settings: dict) -> None:
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    SETTINGS.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def install() -> None:
    settings = _load()
    current = settings.get("statusLine") or {}
    DATA_DIR.mkdir(exist_ok=True)
    SCRIPT.write_text(SCRIPT_BODY_WINDOWS if WINDOWS else SCRIPT_BODY, encoding="utf-8")
    if current.get("command") == _command():
        print("[statusline] 이미 설치되어 있습니다.")
        return
    if SETTINGS.exists():
        shutil.copy2(SETTINGS, BACKUP)
    if current.get("command"):
        _write_next(current["command"])
        print("[statusline] 기존 상태줄 명령은 그대로 이어서 실행됩니다.")
    else:
        NEXT.unlink(missing_ok=True)
        NEXT_CMD.unlink(missing_ok=True)
    settings["statusLine"] = {**current, "type": "command", "command": _command()}
    _save(settings)
    print(f"[statusline] 설치했습니다. Claude Code 가 다음 응답을 하면 {SNAPSHOT} 에 값이 생깁니다.")
    if BACKUP.exists():
        print(f"[statusline] 원래 설정은 {BACKUP} 에 백업했습니다.")


def uninstall() -> None:
    settings = _load()
    current = settings.get("statusLine") or {}
    if current.get("command") != _command():
        print("[statusline] 설치되어 있지 않습니다.")
        return
    if NEXT.exists():
        settings["statusLine"] = {**current, "command": NEXT.read_text(encoding="utf-8").rstrip("\n")}
    else:
        del settings["statusLine"]
    _save(settings)
    for path in (SCRIPT, NEXT, NEXT_CMD):
        path.unlink(missing_ok=True)
    print("[statusline] 제거하고 원래 상태줄 명령으로 되돌렸습니다.")
