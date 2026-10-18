"""OpenAI API 사용량: 조직 Costs / Usage API.

Admin 키(sk-admin-...)가 필요하다. 날짜 경계는 UTC 기준이다.
"""

import json
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

from . import budget, keys
from .fmt import fmt_tokens, fmt_usd

BASE = "https://api.openai.com/v1/organization"


def _get_all(path: str, params: dict, key: str) -> list:
    buckets = []
    page = None
    while True:
        query = dict(params, **({"page": page} if page else {}))
        req = urllib.request.Request(
            f"{BASE}/{path}?{urllib.parse.urlencode(query)}",
            headers={"Authorization": f"Bearer {key}", "User-Agent": "llm-deck/0.1"},
        )
        with urllib.request.urlopen(req, timeout=20) as resp:
            body = json.load(resp)
        buckets += body.get("data", [])
        if not body.get("has_more"):
            return buckets
        page = body["next_page"]


def _spend_limit(key: str) -> float | None:
    """조직 월 지출 한도 (USD). 한도가 없거나 읽을 수 없으면 None."""
    req = urllib.request.Request(
        f"{BASE}/spend_limit", headers={"Authorization": f"Bearer {key}", "User-Agent": "llm-deck/0.1"}
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            body = json.load(resp)
        return float(body["threshold_amount"]) / 100  # 센트 단위
    except (OSError, ValueError, KeyError, TypeError):
        return None


def collect(cfg: dict) -> dict | None:
    key = cfg.get("admin_key")
    if not key:
        return None

    now = datetime.now(timezone.utc)
    today = now.replace(hour=0, minute=0, second=0, microsecond=0)
    month_start = today.replace(day=1)

    try:
        costs = _get_all("costs", {"start_time": int(budget.start(cfg, month_start).timestamp()), "bucket_width": "1d", "limit": 31}, key)
    except urllib.error.HTTPError as e:
        if e.code not in (401, 403):
            raise
        return keys.rejected_page("OpenAI API", "openai_api", key, cfg, e.code, "platform.openai.com > Organization settings > Admin keys")
    keys.mark_ok("openai_api", key)
    # {"YYYY-MM-DD": USD}
    daily = {
        datetime.fromtimestamp(b["start_time"], timezone.utc).strftime("%Y-%m-%d"):
            sum(float(r["amount"]["value"]) for r in b["results"])
        for b in costs
    }
    month = month_start.strftime("%Y-%m")
    month_usd = sum(usd for day, usd in daily.items() if day.startswith(month))
    api_limit = None if cfg.get("monthly_limit") else _spend_limit(key)

    tokens_in = tokens_out = requests = 0
    usage = _get_all("usage/completions", {"start_time": int(today.timestamp()), "bucket_width": "1d"}, key)
    for bucket in usage:
        for r in bucket["results"]:
            tokens_in += r.get("input_tokens", 0)
            tokens_out += r.get("output_tokens", 0)
            requests += r.get("num_model_requests", 0)

    return {
        "name": "OpenAI API",
        "headline": fmt_usd(month_usd),
        "sub": now.strftime("%B") + " so far (UTC)" + keys.expiry_note(cfg),
        "rows": budget.fit(
            [["Cost today", fmt_usd(daily.get(today.strftime("%Y-%m-%d"), 0.0))]],
            budget.rows(cfg, daily, month, api_limit),
            [
                ["Input today", fmt_tokens(tokens_in)],
                ["Output today", fmt_tokens(tokens_out)],
                ["Requests today", str(requests)],
            ],
        ),
    }
