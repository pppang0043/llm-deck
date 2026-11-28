"""공급자가 반환한 USD 사용 비용만 요약한다. 로컬 예산/잔액은 사용하지 않는다."""

from decimal import Decimal


def amount(value) -> Decimal:
    result = Decimal(str(value))
    if not result.is_finite():
        raise ValueError("Invalid provider amount")
    return result


def summary(daily: dict, now) -> dict:
    month, today = now.strftime("%Y-%m"), now.strftime("%Y-%m-%d")
    return {"month": float(sum((v for day, v in daily.items() if day.startswith(month) and day <= today),
                               Decimal(0))),
            "today": float(daily[today]) if today in daily else None,
            "month_period": month, "day_period": today, "currency": "USD",
            "scope": "organization", "as_of": now.isoformat()}
