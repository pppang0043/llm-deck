"""청구액 합계와 월 한도 / 크레딧 남은 금액.

두 회사 모두 선불 크레딧 잔액을 알려주는 API 가 없고, Anthropic 은 월 지출 한도도 API 로 읽을 수 없다.
그래서 config.toml 에 적어 둔 값에서 그 뒤에 쓴 금액(비용 리포트)을 빼서 보여준다.
OpenAI 월 한도는 API(/v1/organization/spend_limit)로 읽고, config 에 적은 값이 있으면 그것을 쓴다.

    monthly_limit = 100        # 월 지출 한도 (USD)
    credit = 99.52             # 콘솔에 보이는 크레딧 잔액 (USD)
    credit_date = "2026-10-18" # 위 잔액을 본 날짜. 이날 쓴 금액부터 뺀다 (이미 빠진 금액이 겹쳐 조금 적게 나올 수 있다)
"""

from datetime import date, datetime, timezone

from . import keys


def _number(value) -> float | None:
    try:
        return float(value) if value not in (None, "") else None
    except (TypeError, ValueError):
        return None


def credit_date(cfg: dict) -> date | None:
    try:
        return date.fromisoformat(str(cfg.get("credit_date", "")).strip())
    except ValueError:
        return None


def start(cfg: dict, month_start: datetime) -> datetime:
    """비용을 읽기 시작할 날 (UTC 자정). 크레딧 날짜가 이번 달보다 앞이면 그날부터."""
    since = credit_date(cfg) if _number(cfg.get("credit")) is not None else None
    if since is None:
        return month_start
    return min(month_start, datetime(since.year, since.month, since.day, tzinfo=timezone.utc))


def summary(cfg: dict, daily: dict[str, float], month_start: datetime, today: datetime,
            api_limit: float | None = None) -> dict:
    """daily: {"YYYY-MM-DD": USD} -> {"month", "today", "limit_left", "credit_left", "note"} (USD, 없으면 None)."""
    month = month_start.strftime("%Y-%m")
    month_usd = sum(usd for day, usd in daily.items() if day.startswith(month))

    limit = _number(cfg.get("monthly_limit"))
    if limit is None:
        limit = api_limit

    credit = _number(cfg.get("credit"))
    since = credit_date(cfg)
    credit_left = None
    if credit is not None and since:
        credit_left = credit - sum(usd for day, usd in daily.items() if day >= since.isoformat())

    return {
        "month": month_usd,
        "today": daily.get(today.strftime("%Y-%m-%d"), 0.0),
        "limit_left": limit - month_usd if limit else None,
        "credit_left": credit_left,
        "note": keys.expiry_note(cfg),
    }
