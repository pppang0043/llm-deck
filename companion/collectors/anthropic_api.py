"""Anthropic API 사용량: Admin API 의 비용/사용량 리포트.

문서: https://platform.claude.com/docs/en/manage-claude/usage-cost-api
Admin API 키가 필요하며, 개인 계정에서는 쓸 수 없다. 날짜 경계는 UTC 기준이다.
"""

import json
import urllib.parse
import urllib.request
from datetime import datetime, timezone

from .fmt import fmt_tokens, fmt_usd

BASE = "https://api.anthropic.com/v1/organizations"


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


def collect(cfg: dict) -> dict | None:
    key = cfg.get("admin_key")
    if not key:
        return None

    now = datetime.now(timezone.utc)
    today = now.replace(hour=0, minute=0, second=0, microsecond=0)
    month_start = today.replace(day=1)
    iso = "%Y-%m-%dT%H:%M:%SZ"

    month_cents = today_cents = 0.0
    for bucket in _get_all("cost_report", {"starting_at": month_start.strftime(iso), "limit": 31}, key):
        cents = sum(float(r["amount"]) for r in bucket["results"])
        month_cents += cents
        if bucket["starting_at"].startswith(today.strftime("%Y-%m-%d")):
            today_cents = cents

    tokens_in = tokens_out = cache_read = 0
    usage = _get_all("usage_report/messages", {"starting_at": today.strftime(iso), "bucket_width": "1d"}, key)
    for bucket in usage:
        for r in bucket["results"]:
            cache_write = sum((r.get("cache_creation") or {}).values())
            tokens_in += r.get("uncached_input_tokens", 0) + cache_write
            tokens_out += r.get("output_tokens", 0)
            cache_read += r.get("cache_read_input_tokens", 0)

    return {
        "name": "Anthropic API",
        "headline": fmt_usd(month_cents / 100),
        "sub": now.strftime("%B") + " so far (UTC)",
        "rows": [
            ["Cost today", fmt_usd(today_cents / 100)],
            ["Input today", fmt_tokens(tokens_in)],
            ["Output today", fmt_tokens(tokens_out)],
            ["Cache read today", fmt_tokens(cache_read)],
        ],
    }
