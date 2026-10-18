"""OpenAI API 청구액: 조직 Costs API 와 월 지출 한도.

Admin 키(sk-admin-...)가 필요하다. 날짜 경계는 UTC 기준이다.
"""

import json
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

from . import budget, keys

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
    while True:
        body = _get(path, dict(params, **({"page": page} if page else {})), key)
        buckets += body.get("data", [])
        if not body.get("has_more"):
            return buckets
        page = body["next_page"]


def _spend_limit(key: str) -> float | None:
    """조직 월 지출 한도 (USD). 한도가 없으면(404) 또는 읽을 수 없으면 None."""
    try:
        return float(_get("spend_limit", {}, key)["threshold_amount"]) / 100  # 센트 단위
    except (OSError, ValueError, KeyError, TypeError):
        return None


def fetch(cfg: dict) -> dict | None:
    """billing.py 가 표로 만드는 값. 키가 없으면 None, 키가 거절되면 {"error": 안내}."""
    key = cfg.get("admin_key")
    if not key:
        return None

    now = datetime.now(timezone.utc)
    today = now.replace(hour=0, minute=0, second=0, microsecond=0)
    month_start = today.replace(day=1)

    try:
        start = int(budget.start(cfg, month_start).timestamp())
        costs = _get_all("costs", {"start_time": start, "bucket_width": "1d", "limit": 31}, key)
    except urllib.error.HTTPError as e:
        if e.code not in (401, 403):
            raise
        return {"error": keys.rejected_text("openai_api", key, cfg, e.code, KEY_PAGE)}
    keys.mark_ok("openai_api", key)
    # {"YYYY-MM-DD": USD}
    daily = {
        datetime.fromtimestamp(b["start_time"], timezone.utc).strftime("%Y-%m-%d"):
            sum(float(r["amount"]["value"]) for r in b["results"])
        for b in costs
    }
    api_limit = None if cfg.get("monthly_limit") else _spend_limit(key)
    return budget.summary(cfg, daily, month_start, today, api_limit)
