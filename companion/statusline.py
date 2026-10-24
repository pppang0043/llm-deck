"""Claude Code 상태줄에 끼워 넣어 한도 사용률을 파일로 남기는 스크립트를 설치/제거한다.

    uv run main.py --install-statusline
    uv run main.py --uninstall-statusline

원래 있던 상태줄 명령은 그대로 이어서 실행되므로 기존 상태줄 표시는 바뀌지 않는다.
"""

import json
import os
import shutil
import sys
from pathlib import Path, PureWindowsPath

DATA_DIR = Path.home() / ".llm-deck"
SNAPSHOT = DATA_DIR / "claude-statusline.json"
WINDOWS = os.name == "nt"
SCRIPT = DATA_DIR / ("statusline.py" if WINDOWS else "statusline.sh")
# 예전 Windows 설치가 쓰던 PowerShell 스크립트. 실행 정책을 우회해(-ExecutionPolicy Bypass) 상태줄마다 PowerShell 을
# 띄우는 것을 회사 보안 프로그램이 의심해서, 이 PC 에 설치된 Python 으로 도는 statusline.py 로 바꿨다
LEGACY_SCRIPT = DATA_DIR / "statusline.ps1"
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

# Windows 용 (이 PC 에 설치된 Python 으로 돈다). 하는 일은 위와 같다.
SCRIPT_BODY_WINDOWS = """# LLM Deck: Claude Code 가 넘겨준 사용 정보를 파일로 남기고, 원래 상태줄 명령을 이어서 실행한다.
import subprocess
import sys
from pathlib import Path

folder = Path.home() / ".llm-deck"
payload = sys.stdin.buffer.read()
if b'"rate_limits"' in payload:
    # 임시 파일을 거치지 않고 바로 쓴다. Claude Code 가 실행 중인 스크립트를 중단시키면 임시 파일이 남기 때문이다.
    # 쓰다 만 파일은 도우미가 건너뛴다.
    try:
        (folder / "claude-statusline.json").write_bytes(payload)
    except OSError:
        pass
nxt = folder / "statusline-next.cmd"
if nxt.exists():
    result = subprocess.run(["cmd", "/d", "/c", str(nxt)], input=payload, stdout=subprocess.PIPE)
    sys.stdout.buffer.write(result.stdout)
"""


def _python() -> str:
    """상태줄을 돌릴 Python. 창 없이 띄우는 pythonw 가 아니라 출력을 돌려주는 python.exe."""
    exe = PureWindowsPath(sys.executable)
    if exe.name.lower() == "pythonw.exe" and os.path.exists(exe.with_name("python.exe")):
        exe = exe.with_name("python.exe")
    return exe.as_posix()


def _command() -> str:
    if WINDOWS:
        # Claude Code 가 cmd 로 돌리든 bash 로 돌리든 같게 읽히도록 / 로 적는다
        return f'"{_python()}" "{SCRIPT.as_posix()}"'
    return f'sh "{SCRIPT}"'


def _ours(command: str | None) -> bool:
    """이 도우미가 넣은 상태줄 명령인지 (예전 PowerShell 판, 다른 Python 경로로 넣은 것 포함)."""
    if not command:
        return False
    return command == _command() or any(n in command.replace("\\", "/") for n in
                                        (LEGACY_SCRIPT.as_posix(), SCRIPT.as_posix()))


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
    if WINDOWS:
        LEGACY_SCRIPT.unlink(missing_ok=True)
    if current.get("command") == _command():
        print("[statusline] 이미 설치되어 있습니다.")
        return
    if SETTINGS.exists():
        shutil.copy2(SETTINGS, BACKUP)
    if _ours(current.get("command")):
        # 예전 판(PowerShell 등)을 새 명령으로 바꾼다. 원래 상태줄 명령(statusline-next)은 그대로 이어서 실행된다
        print("[statusline] 예전 상태줄 명령을 새 명령으로 바꿉니다.")
    elif current.get("command"):
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
    if not _ours(current.get("command")):
        print("[statusline] 설치되어 있지 않습니다.")
        return
    if NEXT.exists():
        settings["statusLine"] = {**current, "command": NEXT.read_text(encoding="utf-8").rstrip("\n")}
    else:
        del settings["statusLine"]
    _save(settings)
    for path in (SCRIPT, LEGACY_SCRIPT, NEXT, NEXT_CMD):
        path.unlink(missing_ok=True)
    print("[statusline] 제거하고 원래 상태줄 명령으로 되돌렸습니다.")
