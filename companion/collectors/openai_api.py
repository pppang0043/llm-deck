"""OpenAI API 사용량: 조직 Costs / Usage API.

Admin 키(sk-admin-...)가 필요하다. 날짜 경계는 UTC 기준이다.
"""

import json
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

from . import keys
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


def collect(cfg: dict) -> dict | None:
    key = cfg.get("admin_key")
    if not key:
        return None

    now = datetime.now(timezone.utc)
    today = now.replace(hour=0, minute=0, second=0, microsecond=0)
    month_start = today.replace(day=1)

    month_usd = today_usd = 0.0
    try:
        costs = _get_all("costs", {"start_time": int(month_start.timestamp()), "bucket_width": "1d", "limit": 31}, key)
    except urllib.error.HTTPError as e:
        if e.code not in (401, 403):
            raise
        return keys.rejected_page("OpenAI API", "openai_api", key, cfg, e.code, "platform.openai.com > Organization settings > Admin keys")
    keys.mark_ok("openai_api", key)
    for bucket in costs:
        usd = sum(float(r["amount"]["value"]) for r in bucket["results"])
        month_usd += usd
        if bucket["start_time"] == int(today.timestamp()):
            today_usd = usd

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
        "rows": [
            ["Cost today", fmt_usd(today_usd)],
            ["Input today", fmt_tokens(tokens_in)],
            ["Output today", fmt_tokens(tokens_out)],
            ["Requests today", str(requests)],
        ],
    }
