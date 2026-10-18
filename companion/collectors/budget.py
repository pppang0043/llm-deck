"""청구액 페이지에 붙이는 월 한도 / 크레딧 남은 금액.

두 회사 모두 선불 크레딧 잔액을 알려주는 API 가 없고, Anthropic 은 월 지출 한도도 API 로 읽을 수 없다.
그래서 config.toml 에 적어 둔 값에서 그 뒤에 쓴 금액(비용 리포트)을 빼서 보여준다.
OpenAI 월 한도는 API(/v1/organization/spend_limit)로 읽고, config 에 적은 값이 있으면 그것을 쓴다.

    monthly_limit = 100        # 월 지출 한도 (USD)
    credit = 99.52             # 콘솔에 보이는 크레딧 잔액 (USD)
    credit_date = "2026-10-18" # 위 잔액을 본 날짜. 이날 쓴 금액부터 뺀다 (이미 빠진 금액이 겹쳐 조금 적게 나올 수 있다)
"""

from datetime import date, datetime, timezone

from .fmt import fmt_usd

# 페이지 한 장에 들어가는 줄 수
MAX_ROWS = 4


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


def rows(cfg: dict, daily: dict[str, float], month: str, api_limit: float | None = None) -> list:
    """daily: {"YYYY-MM-DD": USD}. 한도·크레딧 줄을 만든다. 적어 둔 값이 없으면 빈 목록."""
    result = []
    limit = _number(cfg.get("monthly_limit"))
    if limit is None:
        limit = api_limit
    if limit:
        spent = sum(usd for day, usd in daily.items() if day.startswith(month))
        left = limit - spent
        limit_text = f"${limit:,.0f}" if limit == int(limit) else fmt_usd(limit)
        text = f"{fmt_usd(left)} / {limit_text}" if left >= 0 else f"초과 {fmt_usd(-left)}"
        result.append(["한도 남음", text])

    credit = _number(cfg.get("credit"))
    since = credit_date(cfg)
    if credit is not None and since:
        spent = sum(usd for day, usd in daily.items() if day >= since.isoformat())
        result.append(["크레딧 남음", fmt_usd(credit - spent)])
    return result


def fit(first: list, budget_rows: list, rest: list) -> list:
    """첫 줄, 한도·크레딧 줄, 나머지 순으로 한 페이지에 들어가는 만큼만."""
    return (first + budget_rows + rest)[:MAX_ROWS]
