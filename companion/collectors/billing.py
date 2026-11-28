"""설정된 조직의 API 사용 비용. 조회 실패 시 과거 값/환산액으로 대체하지 않는다."""

import math
from datetime import datetime, timezone

from . import anthropic_api, openai_api
from .fmt import fmt_usd

PROVIDERS = [("anthropic_api", anthropic_api), ("openai_api", openai_api)]


def collect(cfg: dict) -> dict:
    now = datetime.now(timezone.utc)
    columns = []
    for section, module in PROVIDERS:
        try:
            result = module.fetch(cfg.get(section, {}))
        except Exception as e:
            print(f"[{section}] 비용 조회 실패 ({type(e).__name__})")
            result = {"status": "unavailable"}
        if result is None:
            result = {"status": "unavailable" if cfg.get(section, {}).get("admin_key") else "unset"}
        if not isinstance(result, dict):
            result = {"status": "unavailable"}
        columns.append((module.NAME, result))
    return _page(columns, now)


def unavailable() -> dict:
    return _page([(module.NAME, {"status": "unavailable"}) for _, module in PROVIDERS],
                 datetime.now(timezone.utc))


def _page(columns, now):
    def cell(result, field):
        status = result.get("status")
        if status or "error" in result:
            return {"unset": "미설정", "auth": "인증필요"}.get(status, "조회불가")
        if (result.get("currency") != "USD" or result.get("scope") != "organization"
                or result.get("month_period") != now.strftime("%Y-%m")
                or result.get("day_period") != now.strftime("%Y-%m-%d")):
            return "조회불가"
        value = result.get(field)
        if value is None:
            return "집계대기" if field == "today" else "조회불가"
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            return "조회불가"
        return fmt_usd(value)

    return {"name": "API Billing", "title": "API Cost", "note": now.strftime("UTC %H:%M"),
            "table": [[""] + [name for name, _ in columns],
                      [now.strftime("%Y-%m")] + [cell(r, "month") for _, r in columns],
                      ["오늘"] + [cell(r, "today") for _, r in columns]],
            "sub": "설정된 조직 · USD · UTC\nAPI 사용액 · 결제 내역 아님"}
