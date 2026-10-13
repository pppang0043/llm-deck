"""Claude Code 구독 한도 사용률(5시간/주간).

Claude Code 는 상태줄을 그릴 때마다 한도 사용률을 스크립트에 넘겨준다. statusline.py 가 설치한
스크립트가 그 값을 파일로 남기고, 여기서는 그 파일을 읽는다. 값은 계정 기준이라 다른 PC 에서 쓴 양도
포함되지만, 이 PC 에서 Claude Code 가 마지막으로 응답한 시점의 값이다. Pro/Max 구독에서만 나온다.
"""

import json

from statusline import SNAPSHOT

from .fmt import limits_page

WINDOWS = [("five_hour", "5h"), ("seven_day", "Week")]


def collect(cfg: dict) -> dict | None:
    if not cfg.get("enabled", True):
        return None
    try:
        limits = json.loads(SNAPSHOT.read_text(encoding="utf-8"))["rate_limits"]
        as_of = SNAPSHOT.stat().st_mtime
    except (OSError, ValueError, KeyError):
        return {
            "name": "Claude Code",
            "headline": "No data",
            "sub": "run main.py --install-statusline",
        }
    windows = [
        (label, limits[key].get("used_percentage", 0), limits[key].get("resets_at"))
        for key, label in WINDOWS
        if isinstance(limits.get(key), dict)
    ]
    if not windows:
        return None
    return limits_page("Claude Code", windows, as_of)
