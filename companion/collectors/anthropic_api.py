"""Anthropic API 청구액: Admin API 의 비용 리포트.

문서: https://platform.claude.com/docs/en/manage-claude/usage-cost-api
Admin API 키가 필요하며, 개인 계정에서는 쓸 수 없다. 날짜 경계는 UTC 기준이다.
"""

import json
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

from . import budget, keys

NAME = "Anthropic"
BASE = "https://api.anthropic.com/v1/organizations"
KEY_PAGE = "platform.claude.com/settings/admin-keys"


def _get_all(path: str, params: dict, key: str) -> list:
    """모든 페이지의 시간 구간(bucket)을 모아서 돌려준다."""
    buckets = []
    page = None
    while True:
        query = dict(params, **({"page": page} if page else {}))
        req = urllib.request.Request(
            f"{BASE}/{path}?{urllib.parse.urlencode(query)}",
            headers={"x-api-key": key, "anthropic-version": "2023-06-01", "User-Agent": "llm-deck/0.1"},
        )
        with urllib.request.urlopen(req, timeout=20) as resp:
            body = json.load(resp)
        buckets += body.get("data", [])
        if not body.get("has_more"):
            return buckets
        page = body["next_page"]


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
        costs = _get_all("cost_report", {"starting_at": budget.start(cfg, month_start).strftime(iso), "limit": 31}, key)
    except urllib.error.HTTPError as e:
        if e.code not in (401, 403):
            raise
        return {"error": keys.rejected_text("anthropic_api", key, cfg, e.code, KEY_PAGE)}
    keys.mark_ok("anthropic_api", key)
    # {"YYYY-MM-DD": USD}. amount 는 센트 단위
    daily = {b["starting_at"][:10]: sum(float(r["amount"]) for r in b["results"]) / 100 for b in costs}
    return budget.summary(cfg, daily, month_start, today)
