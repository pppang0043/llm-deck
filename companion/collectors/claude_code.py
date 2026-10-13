"""Claude Code 구독 한도 사용률(5시간/주간).

Claude Code 는 상태줄을 그릴 때마다 한도 사용률을 스크립트에 넘겨준다. statusline.py 가 설치한
스크립트가 그 값을 파일로 남기고, 여기서는 그 파일을 읽는다. 값은 계정 기준이라 다른 PC 에서 쓴 양도
포함되지만, 이 PC 에서 Claude Code 가 마지막으로 응답한 시점의 값이다. Pro/Max 구독에서만 나온다.
"""

import json

from statusline import SETTINGS, SNAPSHOT

NAME = "Claude Code"
WINDOWS = [("five_hour", "5h"), ("seven_day", "Week")]


def snapshot(cfg: dict) -> dict | None:
    """{"name": 제목, "as_of": 시각, "windows": [[이름, 사용률 %, 초기화 시각], ...]}. 이 PC 에 값이 없으면 None."""
    try:
        limits = json.loads(SNAPSHOT.read_text(encoding="utf-8"))["rate_limits"]
        as_of = SNAPSHOT.stat().st_mtime
    except (OSError, ValueError, KeyError):
        return None
    windows = [
        [label, limits[key].get("used_percentage", 0), limits[key].get("resets_at")]
        for key, label in WINDOWS
        if isinstance(limits.get(key), dict)
    ]
    return {"name": NAME, "as_of": as_of, "windows": windows} if windows else None


def hint() -> dict | None:
    """값이 어디에도 없을 때 보여줄 안내. Claude Code 를 쓰는 PC 에서만 보여준다."""
    if not SETTINGS.parent.is_dir():
        return None
    return {"name": NAME, "headline": "No data", "sub": "run main.py --install-statusline"}
