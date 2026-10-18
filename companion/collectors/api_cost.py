"""Claude Code / Codex 를 API 로 썼다면 들었을 비용(환산).

구독으로 쓰면 실제 청구액은 월 구독료로 고정이지만, 대화 기록에 남은 토큰 수에 API 단가를 곱하면
"API 로 썼다면 얼마" 를 알 수 있다. 이 PC 의 기록만 센다 (다른 PC 에서 쓴 양은 빠진다).

- Claude Code: ~/.claude/projects/**/*.jsonl 의 assistant 메시지 usage.
  한 응답이 내용 조각마다 여러 줄로 남고, 이어 하기(resume)한 세션은 앞 대화를 새 파일에 다시 적으므로
  메시지 id + 요청 id 로 한 번만 센다. Claude Code 는 기본으로 30일 지난 기록을 지운다.
- Codex: ~/.codex/sessions/**/*.jsonl 의 token_usage_record (응답 하나에 한 줄, response_id 로 한 번만 센다).
  모델은 그 앞의 turn_context 에 적힌 것을 쓴다.

단가는 USD / 100만 토큰. 표에 없는 모델은 비용에 넣지 않고 화면에 "단가 모름" 으로 알린다.
"""

import json
import os
from datetime import datetime
from pathlib import Path

from .fmt import fmt_usd

# Claude: (입력, 출력, 캐시 읽기). 캐시 쓰기는 5분 보관 = 입력 x1.25, 1시간 보관 = 입력 x2.
# platform.claude.com 가격표 (2026-10 기준)
CLAUDE_PRICES = {
    "claude-fable-5-1": (10.0, 50.0, 0.25),
    "claude-mythos-5-1": (10.0, 50.0, 0.25),
    "claude-fable-5": (10.0, 50.0, 1.0),
    "claude-mythos-5": (10.0, 50.0, 1.0),
    "claude-opus-5-5": (4.0, 20.0, 0.20),
    "claude-opus-5": (5.0, 25.0, 0.50),
    "claude-opus-4-8": (5.0, 25.0, 0.50),
    "claude-opus-4-7": (5.0, 25.0, 0.50),
    "claude-opus-4-6": (5.0, 25.0, 0.50),
    "claude-sonnet-5-5": (2.0, 10.0, 0.20),
    "claude-sonnet-5": (2.0, 10.0, 0.20),
    "claude-sonnet-4-6": (3.0, 15.0, 0.30),
    "claude-haiku-5-5": (0.10, 0.50, 0.01),
    "claude-haiku-4-5": (1.0, 5.0, 0.10),
}

# OpenAI: (입력, 캐시 입력, 캐시 쓰기, 출력, 긴 입력일 때 입력 배수, 출력 배수).
# 한 요청의 입력이 272K 를 넘으면 그 요청 전체에 긴 입력 단가가 붙는다. 캐시 단가도 입력 배수만큼 올린다.
# developers.openai.com/api/docs/pricing (2026-10 기준, Standard)
OPENAI_PRICES = {
    "gpt-6-astra": (10.0, 1.00, 12.50, 50.0, 2.0, 1.5),
    "gpt-6.1-sol": (2.0, 0.10, 2.50, 10.0, 2.0, 1.5),
    "gpt-6-sol": (2.0, 0.20, 2.50, 10.0, 2.0, 1.5),
    "gpt-6-luna": (0.10, 0.01, 0.125, 0.50, 1.0, 1.0),
    "gpt-5.3-codex": (1.75, 0.175, 1.75, 14.0, 1.0, 1.0),
}
OPENAI_LONG_INPUT = 272_000

CLAUDE_ROOT = Path(os.environ.get("CLAUDE_CONFIG_DIR", Path.home() / ".claude")) / "projects"
CODEX_ROOT = Path(os.environ.get("CODEX_HOME", Path.home() / ".codex")) / "sessions"

# 파일 경로 -> ((수정 시각, 크기), {중복 막는 키: (날짜, USD 또는 None, 모델)}). 바뀐 파일만 다시 읽는다.
_cache: dict[Path, tuple[tuple, dict]] = {}


def _price(table: dict, model: str):
    """날짜가 붙은 이름(claude-haiku-4-5-20251001 등)도 앞부분이 같으면 찾는다. 긴 이름부터 맞춰 본다."""
    for name in sorted(table, key=len, reverse=True):
        if model == name or model.startswith(name + "-"):
            return table[name]
    return None


def _local_date(timestamp: str) -> str:
    return datetime.fromisoformat(timestamp.replace("Z", "+00:00")).astimezone().strftime("%Y-%m-%d")


def _claude_cost(model: str, usage: dict) -> float | None:
    price = _price(CLAUDE_PRICES, model)
    if not price:
        return None
    rate_in, rate_out, rate_read = price
    write = usage.get("cache_creation") or {}
    write_1h = write.get("ephemeral_1h_input_tokens", 0)
    write_5m = usage.get("cache_creation_input_tokens", 0) - write_1h
    usd = (
        usage.get("input_tokens", 0) * rate_in
        + write_5m * rate_in * 1.25
        + write_1h * rate_in * 2
        + usage.get("cache_read_input_tokens", 0) * rate_read
        + usage.get("output_tokens", 0) * rate_out
    ) / 1_000_000
    # 빠른 모드는 2배
    return usd * 2 if usage.get("speed") == "fast" else usd


def _read_claude(path: Path) -> dict:
    found = {}
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"usage"' not in line:
                continue
            try:
                entry = json.loads(line)
                message = entry["message"]
                usage = message["usage"]
                model = message["model"]
                key = f"{message['id']}:{entry.get('requestId', '')}"
                date = _local_date(entry["timestamp"])
            except (ValueError, KeyError, TypeError):
                continue
            if model.startswith("<") or key in found:  # <synthetic>: Claude Code 가 만든 가짜 응답
                continue
            found[key] = (date, _claude_cost(model, usage), model)
    return found


def _codex_cost(model: str, usage: dict) -> float | None:
    price = _price(OPENAI_PRICES, model)
    if not price:
        return None
    rate_in, rate_cached, rate_write, rate_out, long_in, long_out = price
    total_in = usage.get("input_tokens", 0)
    if total_in > OPENAI_LONG_INPUT:
        rate_in, rate_cached, rate_write, rate_out = (
            rate_in * long_in, rate_cached * long_in, rate_write * long_in, rate_out * long_out
        )
    # input_tokens 는 캐시로 읽은 것과 캐시에 쓴 것까지 포함한 수, output_tokens 는 추론 토큰을 포함한 수
    cached = usage.get("cached_input_tokens", 0)
    written = usage.get("cache_write_input_tokens", 0)
    return (
        (total_in - cached - written) * rate_in
        + cached * rate_cached
        + written * rate_write
        + usage.get("output_tokens", 0) * rate_out
    ) / 1_000_000


def _read_codex(path: Path) -> dict:
    found = {}
    model = ""
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"turn_context"' not in line and '"token_usage_record"' not in line:
                continue
            try:
                entry = json.loads(line)
                payload = entry["payload"]
                if entry.get("type") == "turn_context":
                    model = payload.get("model") or model
                    continue
                if entry.get("type") != "token_usage_record":
                    continue
                key = payload["response_id"]
                usage = payload["usage"]
                date = _local_date(entry["timestamp"])
            except (ValueError, KeyError, TypeError):
                continue
            found[key] = (date, _codex_cost(model, usage), model or "?")
    return found


def _records(root: Path, reader) -> dict:
    """root 아래 모든 기록을 {키: (날짜, USD, 모델)} 로 합친다."""
    merged = {}
    if not root.is_dir():
        return merged
    for path in root.rglob("*.jsonl"):
        try:
            stat = path.stat()
            stamp = (stat.st_mtime_ns, stat.st_size)
            cached = _cache.get(path)
            if not cached or cached[0] != stamp:
                cached = (stamp, reader(path))
                _cache[path] = cached
        except OSError:
            continue
        merged.update(cached[1])
    return merged


def _sum(records: dict, today: str, month: str) -> tuple[float, float, set]:
    day_usd = month_usd = 0.0
    unknown = set()
    for date, usd, model in records.values():
        if not date.startswith(month):
            continue
        if usd is None:
            unknown.add(model)
            continue
        month_usd += usd
        if date == today:
            day_usd += usd
    return day_usd, month_usd, unknown


def collect(cfg: dict) -> dict | None:
    if not cfg.get("api_cost", {}).get("enabled", True):
        return None
    now = datetime.now()
    today, month = now.strftime("%Y-%m-%d"), now.strftime("%Y-%m")
    services = [("Claude", _records(CLAUDE_ROOT, _read_claude)), ("Codex", _records(CODEX_ROOT, _read_codex))]
    services = [(name, records) for name, records in services if records]
    if not services:
        return None

    # 회사별로 나란히 한 표에 (API Billing 화면과 같은 모양). 스크롤 없이 한 화면에 들어가게 한다.
    header, day_row, month_row = [""], ["오늘"], [f"{now.month}월"]
    total = 0.0
    unknown = set()
    for name, records in services:
        day_usd, month_usd, missing = _sum(records, today, month)
        total += month_usd
        unknown |= missing
        header.append(name)
        day_row.append(fmt_usd(day_usd))
        month_row.append(fmt_usd(month_usd))

    # 설명 줄을 빼야 가로 화면에 다 들어간다. 이 PC 기록만 센다는 것은 시각 옆에 적는다.
    # side: 기기에서 앞 페이지(구독제) 오른쪽에 둔다 (아래로 넘기면 그다음 페이지)
    page = {"name": "API Cost", "note": now.strftime("이 PC · %H:%M"), "headline": fmt_usd(total),
            "table": [header, month_row, day_row], "side": True}
    if unknown:
        page["sub"] = "단가 모름: " + ", ".join(sorted(unknown))
    return page
