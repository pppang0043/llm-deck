"""비정상 종료 뒤 남은 config.toml [orca] usage = false 를 기동 때 정리한다.

ORCA 재시작 등으로 도우미가 비정상 종료하면, 수동으로 usage = false 를 지우고
install.py 를 다시 돌리던 경우가 있다. 앞 도우미 PID 가 이미 죽었으면 stale 로 보고
설정을 되돌려 다시 설치하지 않아도 연결·사용량 수집이 되게 한다.
의도적으로 usage = false 를 두고 정상 종료한 경우는 PID 파일이 없어 건드리지 않는다.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

PID_FILE = Path.home() / ".llm-deck" / "companion.pid"
CONFIG_NAME = "config.toml"


def pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # 다른 사용자 소유. 살아 있는 것으로 본다
    except OSError:
        return False


def previous_helper_crashed() -> bool:
    """앞 도우미 PID 파일이 있는데 프로세스가 없으면 비정상 종료로 본다."""
    try:
        pid = int(PID_FILE.read_text(encoding="utf-8").strip())
    except (OSError, ValueError):
        return False
    if pid == os.getpid() or pid_alive(pid):
        return False
    try:
        PID_FILE.unlink()
    except OSError:
        pass
    return True


def mark_running() -> None:
    try:
        PID_FILE.parent.mkdir(parents=True, exist_ok=True)
        PID_FILE.write_text(str(os.getpid()), encoding="utf-8")
    except OSError:
        pass


def clear_pid() -> None:
    try:
        if PID_FILE.exists() and PID_FILE.read_text(encoding="utf-8").strip() == str(os.getpid()):
            PID_FILE.unlink()
    except OSError:
        pass


def clear_orca_usage_false(path: Path) -> bool:
    """[orca] 칸의 usage = false 줄을 제거한다. 바꿨으면 True."""
    try:
        raw = path.read_bytes()
    except OSError:
        return False
    text = raw.decode("utf-8-sig")
    sections = re.split(r"(?=^\[[^\]]+\]\s*$)", text, flags=re.MULTILINE)
    changed = False
    out = []
    for block in sections:
        if re.match(r"^\[orca\]\s*$", block, flags=re.MULTILINE):
            new_block, n = re.subn(
                r"(?m)^[ \t]*usage[ \t]*=[ \t]*false[ \t]*(?:#.*)?\r?\n?",
                "",
                block,
                count=0,
            )
            if n:
                changed = True
                block = new_block
        out.append(block)
    if not changed:
        return False
    try:
        path.write_text("".join(out), encoding="utf-8")
    except OSError:
        return False
    return True


def recover_stale_orca_usage(config_path: Path, cfg: dict, *, crashed: bool) -> dict:
    """비정상 종료 뒤 남은 [orca] usage = false 를 지우고 cfg 도 맞춘다."""
    if not crashed:
        return cfg
    orca_cfg = cfg.get("orca") or {}
    if orca_cfg.get("usage", True) is not False:
        return cfg
    if not clear_orca_usage_false(config_path):
        return cfg
    print("[device] 비정상 종료 뒤 남은 config.toml [orca] usage = false 를 지웠습니다.")
    updated = dict(orca_cfg)
    updated.pop("usage", None)
    return {**cfg, "orca": updated}
