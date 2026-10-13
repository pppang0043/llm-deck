"""화면에 보여줄 숫자 형식. 기기 글꼴에 영문/숫자만 있으므로 ASCII 만 쓴다."""

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
    return when.strftime("%b %d %H:%M")


def limit_bar(label: str, used_pct: float, resets_at: float | None) -> dict:
    """구독 한도 한 구간(5시간/주간)을 막대 하나로 만든다. 초기화 시각이 지났으면 0% 로 본다."""
    if resets_at and resets_at <= time.time():
        return {"label": label, "note": "", "text": "0%", "pct": 0}
    pct = max(0, min(100, round(used_pct)))
    return {"label": label, "note": fmt_when(resets_at) if resets_at else "", "text": f"{pct}%", "pct": pct}


def limits_page(name: str, snapshot: dict) -> dict:
    """한도 사용률 페이지. snapshot 은 {"as_of": 시각, "windows": [[이름, 사용률 %, 초기화 시각], ...]}."""
    return {
        "name": name,
        "note": fmt_when(snapshot["as_of"]),
        "bars": [limit_bar(*w) for w in snapshot["windows"]],
    }
