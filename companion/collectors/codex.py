"""Codex(ChatGPT 구독) 한도 사용률(5시간/주간).

Codex CLI 는 응답마다 서버가 알려준 한도 사용률을 세션 로그(~/.codex/sessions)에 남긴다.
가장 최근 기록 하나만 읽는다. 값은 계정 기준이지만, 이 PC 에서 Codex 를 마지막으로 쓴 시점의 값이다.
"""

import json
import os
from datetime import datetime
from pathlib import Path

from .fmt import limits_page

# 최근 파일 몇 개만 본다 (가장 최근 세션에 기록이 아직 없을 수 있다)
MAX_FILES = 5


def _label(window: dict) -> str:
    minutes = window.get("window_minutes") or 0
    if minutes == 300:
        return "5h"
    if minutes == 10080:
        return "Week"
    return f"{minutes // 60}h"


def _latest(path: Path) -> tuple[float, dict] | None:
    """파일에서 한도 정보가 담긴 마지막 기록을 찾는다."""
    found = None
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"rate_limits"' not in line:
                continue
            try:
                entry = json.loads(line)
                limits = entry["payload"]["rate_limits"]
                ts = datetime.fromisoformat(entry["timestamp"].replace("Z", "+00:00")).timestamp()
            except (ValueError, KeyError, TypeError):
                continue
            if isinstance(limits, dict) and isinstance(limits.get("primary"), dict):
                found = (ts, limits)
    return found


def collect(cfg: dict) -> dict | None:
    if not cfg.get("enabled", True):
        return None
    root = Path(os.environ.get("CODEX_HOME", Path.home() / ".codex")) / "sessions"
    if not root.is_dir():
        return None
    files = sorted(root.rglob("*.jsonl"), key=lambda p: p.stat().st_mtime, reverse=True)
    for path in files[:MAX_FILES]:
        latest = _latest(path)
        if not latest:
            continue
        as_of, limits = latest
        windows = [
            (_label(w), w.get("used_percent", 0), w.get("resets_at"))
            for w in (limits.get("primary"), limits.get("secondary"))
            if isinstance(w, dict)
        ]
        return limits_page("Codex", windows, as_of)
    return None
