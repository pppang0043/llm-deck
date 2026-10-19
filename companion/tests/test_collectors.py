import json
import time
from datetime import datetime, timezone

import pytest

from collectors import api_cost, fmt


# --- fmt ---

@pytest.mark.parametrize("n, text", [
    (0, "0"), (999, "999"), (1500, "1.5K"), (2_500_000, "2.5M"), (3_000_000_000, "3.00B"),
])
def test_fmt_tokens(n, text):
    assert fmt.fmt_tokens(n) == text


def test_fmt_usd():
    assert fmt.fmt_usd(1234.5) == "$1,234.50"
    assert fmt.fmt_usd(0) == "$0.00"


def test_fmt_when_today_and_other_day():
    now = time.time()
    assert fmt.fmt_when(now) == datetime.fromtimestamp(now).strftime("%H:%M")
    old = datetime(2020, 3, 5, 9, 7).timestamp()
    assert fmt.fmt_when(old) == "3월 5일 09:07"


def test_limit_bar():
    future = time.time() + 3600
    bar = fmt.limit_bar("5h", 150, future)
    assert bar["label"] == "5시간" and bar["pct"] == 100 and bar["text"] == "100%"
    assert bar["resets"] == int(future)
    past = fmt.limit_bar("Week", 80, time.time() - 10)
    assert past["pct"] == 0 and past["text"] == "0%"
    assert fmt.limit_bar("Other", -5, None) == {"label": "Other", "note": "", "text": "0%", "pct": 0}


def test_limits_page_fills_windows_in_order():
    page = fmt.limits_page("Claude", {"as_of": time.time(), "windows": [["Opus", 10, None], ["Week", 50, None]]})
    assert [b["label"] for b in page["bars"]] == ["5시간", "주간", "Opus"]
    assert [b["pct"] for b in page["bars"]] == [0, 50, 10]


# --- api_cost ---

def test_price_prefix_match():
    assert api_cost._price(api_cost.CLAUDE_PRICES, "claude-haiku-4-5-20251001") == (1.0, 5.0, 0.10)
    # claude-opus-5 이 claude-opus-5-5 를 가로채지 않는다 (긴 이름부터)
    assert api_cost._price(api_cost.CLAUDE_PRICES, "claude-opus-5-5") == (4.0, 20.0, 0.20)
    assert api_cost._price(api_cost.CLAUDE_PRICES, "claude-opus-5-20260101") == (5.0, 25.0, 0.50)
    assert api_cost._price(api_cost.CLAUDE_PRICES, "claude-opus-55") is None
    assert api_cost._price(api_cost.CLAUDE_PRICES, "gpt-6-sol") is None


def test_claude_cost_math():
    usage = {"input_tokens": 1_000_000, "output_tokens": 100_000, "cache_read_input_tokens": 1_000_000,
             "cache_creation_input_tokens": 1_000_000, "cache_creation": {"ephemeral_1h_input_tokens": 400_000}}
    # opus-5-5: 입력 4 + 출력 2 + 캐시 읽기 0.2 + 5분 쓰기 0.6M*4*1.25=3 + 1시간 쓰기 0.4M*4*2=3.2
    assert api_cost._claude_cost("claude-opus-5-5", usage) == pytest.approx(12.4)
    assert api_cost._claude_cost("claude-opus-5-5", {**usage, "speed": "fast"}) == pytest.approx(24.8)
    assert api_cost._claude_cost("claude-unknown", usage) is None


def test_codex_cost_math_and_long_input():
    short = {"input_tokens": 100_000, "cached_input_tokens": 50_000, "output_tokens": 10_000}
    # gpt-6-sol: 50K*2 + 50K*0.2 + 10K*10
    assert api_cost._codex_cost("gpt-6-sol", short) == pytest.approx(0.1 + 0.01 + 0.1)
    long = {"input_tokens": 300_000, "cached_input_tokens": 100_000, "output_tokens": 1000}
    # 272K 를 넘으면 입력 x2, 출력 x1.5
    assert api_cost._codex_cost("gpt-6-sol", long) == pytest.approx(200_000 * 4e-6 + 100_000 * 0.4e-6 + 1000 * 15e-6)
    assert api_cost._codex_cost("gpt-unknown", short) is None


def now_iso():
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def claude_line(mid, model, inp, req="r"):
    return json.dumps({"timestamp": now_iso(), "requestId": req,
                       "message": {"id": mid, "model": model, "usage": {"input_tokens": inp}}})


@pytest.fixture
def roots(tmp_path, monkeypatch):
    claude, codex = tmp_path / "claude", tmp_path / "codex"
    monkeypatch.setattr(api_cost, "CLAUDE_ROOT", claude)
    monkeypatch.setattr(api_cost, "CODEX_ROOT", codex)
    monkeypatch.setattr(api_cost, "_cache", {})
    return claude, codex


def test_collect_none_without_records(roots):
    assert api_cost.collect({}) is None
    assert api_cost.collect({"api_cost": {"enabled": False}}) is None


def test_collect_dedupes_and_flags_unknown(roots):
    claude, codex = roots
    (claude / "p").mkdir(parents=True)
    lines = [
        claude_line("m1", "claude-opus-5-5", 1_000_000),
        claude_line("m1", "claude-opus-5-5", 1_000_000),          # 같은 응답의 다음 조각: 한 번만
        claude_line("m2", "claude-sonnet-5", 500_000),
        claude_line("m3", "<synthetic>", 9_000_000),               # 가짜 응답은 뺀다
        claude_line("m4", "claude-mystery-1", 1_000),              # 단가 모름
        "not json {\"usage\"",
    ]
    (claude / "p/a.jsonl").write_text("\n".join(lines) + "\n")
    # 이어 하기한 세션은 앞 대화를 새 파일에 다시 적는다
    (claude / "p/b.jsonl").write_text(claude_line("m1", "claude-opus-5-5", 1_000_000) + "\n")
    (codex / "2026").mkdir(parents=True)
    codex_lines = [
        json.dumps({"type": "turn_context", "timestamp": now_iso(), "payload": {"model": "gpt-6-sol"}}),
        json.dumps({"type": "token_usage_record", "timestamp": now_iso(),
                    "payload": {"response_id": "x1", "usage": {"input_tokens": 200_000}}}),
    ]
    (codex / "2026/s.jsonl").write_text("\n".join(codex_lines) + "\n")

    page = api_cost.collect({})
    assert page["table"][0] == ["", "Claude", "Codex"]
    # Claude: 4 (opus-5-5) + 1 (sonnet-5 0.5M*2) = 5, Codex: 0.2M*2 = 0.4
    assert page["table"][1][1:] == ["$5.00", "$0.40"]
    assert page["table"][2][1:] == ["$5.00", "$0.40"]
    assert page["headline"] == "$5.40"
    assert page["sub"] == "단가 모름: claude-mystery-1"
