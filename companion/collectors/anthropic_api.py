"""Anthropic 조직 Admin API 의 사용 비용 (카드 결제/최종 청구서 아님).

문서: https://platform.claude.com/docs/en/api/beta/organization/analytics/cost/list
Admin API 키가 필요하며, 개인 계정에서는 쓸 수 없다. 날짜 경계는 UTC 기준이다.
"""

import json
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

from . import keys, provider_costs

NAME = "Anthropic"
BASE = "https://api.anthropic.com/v1/organizations"
KEY_PAGE = "platform.claude.com/settings/admin-keys"
API_PRODUCTS = ("messages_api", "message_batches_api", "managed_agents_api")


def _get_all(path: str, params: dict, key: str) -> list:
    """모든 페이지의 시간 구간(bucket)을 모아서 돌려준다."""
    buckets = []
    page = None
    seen = set()
    while True:
        query = dict(params, **({"page": page} if page else {}))
        req = urllib.request.Request(
            f"{BASE}/{path}?{urllib.parse.urlencode(query, doseq=True)}",
            headers={"x-api-key": key, "anthropic-version": "2026-10-21",
                     "anthropic-beta": "analytics-usage-cost-2026-08-24", "User-Agent": "llm-deck/0.1"},
        )
        with urllib.request.urlopen(req, timeout=20) as resp:
            body = json.load(resp)
        if not isinstance(body["data"], list) or not isinstance(body["has_more"], bool):
            raise ValueError("Invalid cost response")
        if not body["data"] and not body.get("data_refreshed_at"):
            raise ValueError("No provider cost export available")
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
    iso = "%Y-%m-%dT%H:%M:%SZ"

    try:
        costs = _get_all("analytics/cost", {"starting_at": month_start.strftime(iso),
                                           "ending_at": now.strftime(iso), "time_bucket_width": "1d",
                                           "group_by[]": ["product"], "products[]": list(API_PRODUCTS),
                                           "limit": 1000}, key)
    except urllib.error.HTTPError as e:
        if e.code not in (401, 403):
            raise
        return {"error": keys.rejected_text("anthropic_api", key, cfg, e.code, KEY_PAGE), "status": "auth"}
    # analytics의 cost.amount는 할인 후/크레딧 차감 전 fractional cents.
    daily = {}
    seen_rows = set()
    for row in costs:
        product = row["group"]["product"]
        if product not in API_PRODUCTS or row["cost"]["currency"].upper() != "USD":
            raise ValueError("Unexpected API product or currency")
        day = datetime.fromisoformat(row["starting_at"].replace("Z", "+00:00")).astimezone(timezone.utc).strftime("%Y-%m-%d")
        identity = (day, product)
        if identity in seen_rows:
            raise ValueError("Duplicate API cost row")
        seen_rows.add(identity)
        daily[day] = daily.get(day, provider_costs.amount(0)) + provider_costs.amount(row["cost"]["amount"]) / 100
    keys.mark_ok("anthropic_api", key)
    return provider_costs.summary(daily, now)
