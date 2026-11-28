"""OpenAI 조직 Costs API 의 사용 비용 (카드 결제/최종 청구서 아님).

Admin 키(sk-admin-...)가 필요하다. 날짜 경계는 UTC 기준이다.
"""

import json
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

from . import keys, provider_costs

NAME = "OpenAI"
BASE = "https://api.openai.com/v1/organization"
KEY_PAGE = "platform.openai.com > Organization settings > Admin keys"


def _get(path: str, params: dict, key: str) -> dict:
    query = f"?{urllib.parse.urlencode(params)}" if params else ""
    req = urllib.request.Request(
        f"{BASE}/{path}{query}", headers={"Authorization": f"Bearer {key}", "User-Agent": "llm-deck/0.1"}
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.load(resp)


def _get_all(path: str, params: dict, key: str) -> list:
    buckets = []
    page = None
    seen = set()
    while True:
        body = _get(path, dict(params, **({"page": page} if page else {})), key)
        if not isinstance(body["data"], list) or not isinstance(body["has_more"], bool):
            raise ValueError("Invalid cost response")
        buckets += body["data"]
        if not body["has_more"]:
            return buckets
        next_page = body["next_page"]
        if not next_page or next_page in seen:
            raise ValueError("Invalid cost pagination")
        seen.add(next_page)
        page = next_page


def fetch(cfg: dict) -> dict | None:
    """billing.py 가 표로 만드는 값. 키가 없으면 None, 키가 거절되면 {"error": 안내}."""
    key = cfg.get("admin_key")
    if not key:
        return None

    now = datetime.now(timezone.utc)
    today = now.replace(hour=0, minute=0, second=0, microsecond=0)
    month_start = today.replace(day=1)

    try:
        costs = _get_all("costs", {"start_time": int(month_start.timestamp()),
                                  "end_time": int(now.timestamp()), "bucket_width": "1d", "limit": 31}, key)
    except urllib.error.HTTPError as e:
        if e.code not in (401, 403):
            raise
        return {"error": keys.rejected_text("openai_api", key, cfg, e.code, KEY_PAGE), "status": "auth"}
    # {"YYYY-MM-DD": USD}
    daily = {}
    for bucket in costs:
        day = datetime.fromtimestamp(bucket["start_time"], timezone.utc).strftime("%Y-%m-%d")
        if day in daily:
            raise ValueError("Duplicate cost bucket")
        values = []
        for result in bucket["results"]:
            if result["amount"]["currency"].lower() != "usd":
                raise ValueError("Unsupported cost currency")
            values.append(provider_costs.amount(result["amount"]["value"]))
        daily[day] = sum(values, provider_costs.amount(0))
    keys.mark_ok("openai_api", key)
    return provider_costs.summary(daily, now)
