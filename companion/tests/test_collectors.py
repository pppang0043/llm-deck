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
    assert "headline" not in page
    assert page["title"] == "API Estimate"
    assert "청구액 아님" in page["sub"] and "구독 사용 포함" in page["sub"]
    assert page["sub"].endswith("단가 모름: claude-mystery-1")


def test_sonnet_55_cache_hit_rate():
    assert api_cost._claude_cost("claude-sonnet-5-5", {
        "cache_read_input_tokens": 1_000_000,
    }) == pytest.approx(0.10)


def test_read_only_audit_excludes_old_month_and_marks_unknown(roots):
    from audit_api_cost import audit

    claude, codex = roots
    claude.mkdir()
    old = json.loads(claude_line("old", "claude-opus-5-5", 1_000_000))
    old["timestamp"] = "2026-10-15T00:00:00Z"
    unknown = json.loads(claude_line("unknown", "claude-mystery", 100))
    unknown["timestamp"] = "2026-11-15T00:00:00Z"
    (claude / "a.jsonl").write_text(json.dumps(old) + "\n" + json.dumps(unknown) + "\n")
    report = audit("2026-11", claude, codex)
    assert report["services"]["Claude"]["models"] == {
        "claude-mystery": {"responses": 1, "estimated_usd": None}}
    assert "NOT a bill" in report["kind"]


def test_claude_streaming_and_resumed_copy_use_complete_snapshot(roots):
    claude, _ = roots
    claude.mkdir()
    small = claude_line("m1", "claude-opus-5-5", 100)
    complete = claude_line("m1", "claude-opus-5-5", 1_000_000)
    (claude / "a.jsonl").write_text(small + "\n" + complete + "\n")
    (claude / "b.jsonl").write_text(small + "\n")
    assert api_cost.collect({})["table"][1][1] == "$4.00"
    # 더 작은 복사본이 나중에 다시 수집돼도 줄어들지 않는다.
    (claude / "b.jsonl").write_text(small + "\n\n")
    assert api_cost.collect({})["table"][1][1] == "$4.00"
    (claude / "a.jsonl").unlink()
    # 지워진 파일의 캐시를 계속 합산하지 않는다.
    assert api_cost.collect({})["table"][1][1] == "$0.00"


def test_month_filter_does_not_include_previous_month_or_unknown_prices():
    records = {"old": ("2026-10-31", 999, "known"),
               "start": ("2026-11-01", 1.234567, "known"),
               "today": ("2026-11-28", 2.345678, "known"),
               "unknown": ("2026-11-28", None, "unknown")}
    day, month, unknown = api_cost._sum(records, "2026-11-28", "2026-11")
    assert day == pytest.approx(2.345678)
    assert month == pytest.approx(3.580245)
    assert unknown == {"unknown"}


def test_resumed_copy_does_not_move_original_response_to_next_month(roots):
    claude, _ = roots
    claude.mkdir()
    original = json.loads(claude_line("m1", "claude-opus-5-5", 1_000_000))
    original["timestamp"] = "2026-11-15T00:00:00Z"
    copy = {**original, "timestamp": "2026-12-15T00:00:00Z"}
    (claude / "a.jsonl").write_text(json.dumps(original) + "\n")
    (claude / "b.jsonl").write_text(json.dumps(copy) + "\n")
    records = api_cost._records(claude, api_cost._read_claude)
    assert api_cost._sum(records, "2026-11-15", "2026-11")[1] == pytest.approx(4)
    assert api_cost._sum(records, "2026-12-15", "2026-12")[1] == 0


@pytest.mark.skipif(not hasattr(time, "tzset"), reason="tzset unavailable on Windows")
def test_local_month_boundary(monkeypatch):
    try:
        with monkeypatch.context() as patch:
            patch.setenv("TZ", "Asia/Seoul")
            time.tzset()
            assert api_cost._local_date("2026-10-31T15:00:00Z") == "2026-11-01"
            assert api_cost._local_date("2026-10-31T14:59:59Z") == "2026-10-31"
    finally:
        time.tzset()
