"""Claude Code 사용량: 로컬 세션 로그(~/.claude/projects/**/*.jsonl)의 토큰 수를 집계한다.

로그에는 금액이 없으므로 토큰 수만 보여준다. API 키가 필요 없다.
"""

import json
import os
import time
from collections import Counter
from datetime import datetime
from pathlib import Path

from .fmt import fmt_tokens

WEEK = 7 * 24 * 3600

# 파일 경로 -> (수정 시각, 크기, 레코드 목록). 바뀌지 않은 파일은 다시 읽지 않는다.
_cache: dict[Path, tuple[float, int, list]] = {}


def _parse(path: Path) -> list:
    records = []
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"usage"' not in line:
                continue
            try:
                entry = json.loads(line)
                msg = entry["message"]
                usage = msg["usage"]
                ts = datetime.fromisoformat(entry["timestamp"].replace("Z", "+00:00")).timestamp()
            except (ValueError, KeyError, TypeError):
                continue
            model = msg.get("model") or ""
            if model.startswith("<"):
                continue
            records.append(
                (
                    msg.get("id") or entry.get("uuid"),
                    ts,
                    model,
                    usage.get("input_tokens", 0),
                    usage.get("output_tokens", 0),
                    usage.get("cache_creation_input_tokens", 0),
                    usage.get("cache_read_input_tokens", 0),
                )
            )
    return records


def collect(cfg: dict) -> dict | None:
    if not cfg.get("enabled", True):
        return None
    root = Path(os.environ.get("CLAUDE_CONFIG_DIR", Path.home() / ".claude")) / "projects"
    if not root.is_dir():
        return None

    now = time.time()
    # 응답 하나가 로그에 여러 줄로 남으므로 메시지 id 로 중복을 없앤다 (마지막 줄이 최종 값)
    messages = {}
    for path in root.rglob("*.jsonl"):
        stat = path.stat()
        if stat.st_mtime < now - WEEK:
            continue
        cached = _cache.get(path)
        if not cached or cached[:2] != (stat.st_mtime, stat.st_size):
            cached = _cache[path] = (stat.st_mtime, stat.st_size, _parse(path))
        for record in cached[2]:
            messages[record[0]] = record

    midnight = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
    today_new = today_out = today_cache_read = today_count = 0
    last_5h = last_week = 0
    models = Counter()
    for _, ts, model, inp, out, cache_write, cache_read in messages.values():
        if ts < now - WEEK:
            continue
        # 캐시 읽기는 양이 매우 크고 저렴해서 합계에서 빼고 따로 보여준다
        new_tokens = inp + out + cache_write
        last_week += new_tokens
        if ts >= now - 5 * 3600:
            last_5h += new_tokens
        if ts >= midnight:
            today_new += new_tokens
            today_out += out
            today_cache_read += cache_read
            today_count += 1
            models[model] += new_tokens

    rows = [
        ["Output today", fmt_tokens(today_out)],
        ["Cache read today", fmt_tokens(today_cache_read)],
        ["Last 5 hours", fmt_tokens(last_5h)],
        ["Last 7 days", fmt_tokens(last_week)],
    ]
    if models:
        rows.append(["Top model", models.most_common(1)[0][0].removeprefix("claude-")])
    return {
        "name": "Claude Code",
        "headline": f"{fmt_tokens(today_new)} tok",
        "sub": f"today, {today_count} responses",
        "rows": rows,
    }
