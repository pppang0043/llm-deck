"""화면에 보여줄 글자 형식. 기기 글꼴에는 영문/숫자와 자주 쓰는 한글이 들어 있다."""

import time
from datetime import datetime


def fmt_tokens(n: float) -> str:
    if n >= 1_000_000_000:
        return f"{n / 1_000_000_000:.2f}B"
    if n >= 1_000_000:
        return f"{n / 1_000_000:.1f}M"
    if n >= 1_000:
        return f"{n / 1_000:.1f}K"
    return str(int(n))


def fmt_usd(amount: float) -> str:
    return f"${amount:,.2f}"


def fmt_when(ts: float) -> str:
    """오늘이면 시각만, 아니면 날짜까지."""
    when = datetime.fromtimestamp(ts)
    if when.date() == datetime.now().date():
        return when.strftime("%H:%M")
    return f"{when.month}월 {when.day}일 {when:%H:%M}"


# 저장소에 적는 구간 이름(여러 PC 가 같은 이름으로 맞춰 쓴다) -> 화면에 보일 이름.
# 이 구간들은 값이 없어도(초기화된 뒤 아직 안 썼으면 서버가 알려주지 않는다) 0% 로 항상 보여준다.
WINDOW_LABELS = {"5h": "5시간", "Week": "주간"}


def limit_bar(label: str, used_pct: float, resets_at: float | None) -> dict:
    """구독 한도 한 구간(5시간/주간)을 막대 하나로 만든다. 초기화 시각이 지났으면 0% 로 본다."""
    if resets_at and resets_at <= time.time():
        return {"label": WINDOW_LABELS.get(label, label), "note": "", "text": "0%", "pct": 0}
    pct = max(0, min(100, round(used_pct)))
    # 기기에서 두 서비스를 한 표에 나란히 보이므로 짧게: 오늘이면 시각만, 아니면 "23일 15:00"
    when = datetime.fromtimestamp(resets_at) if resets_at else None
    note = "" if not when else when.strftime("%H:%M") if when.date() == datetime.now().date() \
        else f"{when.day}일 {when:%H:%M}"
    return {"label": WINDOW_LABELS.get(label, label), "note": note, "text": f"{pct}%", "pct": pct}


def _with_all_windows(windows: list) -> list:
    """빠진 구간을 0% 로 채우고 5시간, 주간 순서로 맞춘다."""
    by_label = {w[0]: w for w in windows}
    filled = [by_label.get(label, [label, 0, None]) for label in WINDOW_LABELS]
    return filled + [w for w in windows if w[0] not in WINDOW_LABELS]


def limits_page(name: str, snapshot: dict) -> dict:
    """한도 사용률 페이지. snapshot 은 {"as_of": 시각, "windows": [[이름, 사용률 %, 초기화 시각], ...]}."""
    return {
        "name": name,
        "note": fmt_when(snapshot["as_of"]),
        "bars": [limit_bar(*w) for w in _with_all_windows(snapshot["windows"])],
    }
