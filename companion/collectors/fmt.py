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
        return {"label": label, "text": "0% (reset)", "pct": 0}
    text = f"{used_pct:.0f}%"
    if resets_at:
        text += f", resets {fmt_when(resets_at)}"
    return {"label": label, "text": text, "pct": max(0, min(100, round(used_pct)))}


def limits_page(name: str, windows: list[tuple[str, float, float | None]], as_of: float, rows: list) -> dict:
    """한도 사용률 페이지. windows 는 (이름, 사용률 %, 초기화 시각) 목록이고 첫 번째가 큰 글자로 나온다."""
    bars = [limit_bar(*w) for w in windows]
    return {
        "name": name,
        "headline": f"{bars[0]['pct']}%",
        "sub": f"of {windows[0][0]} limit used",
        "bars": bars,
        "rows": rows + [["As of", fmt_when(as_of)]],
    }
