import json
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path

import pytest

from collectors import api_cost, usage_estimate


@pytest.fixture
def scoped(tmp_path, monkeypatch):
    now = datetime(2026, 11, 28, 12, tzinfo=timezone.utc).timestamp()
    root = tmp_path / "shared-sessions"
    root.mkdir()
    home = tmp_path / "home"
    home.mkdir(exist_ok=True)
    (home / "sessions").symlink_to(root, target_is_directory=True)
    data = {"codex": {"activeAccountId": "selected", "accounts": [
        {"id": "selected", "home": str(home), "providerAccountId": "identity", "workspaceAccountId": "workspace"}]}}
    pages = [{"name": "Codex selected", "estimate_source": {"provider": "codex", "as_of": now, "title": "Codex"},
              "bars": [{"label": label, "resets": now + 3600, "pct": 97} for label in ("5시간", "주간")]}]
    monkeypatch.setattr(usage_estimate.time, "time", lambda: now)
    monkeypatch.setattr(api_cost, "_cache", {})
    monkeypatch.setattr(usage_estimate, "_scope_cache", {})

    def log(name, identity="identity", account_scoped=True, events=None, model="gpt-6.1-sol"):
        lines = [{"type": "session_meta", "payload": {"creator_account_id": identity, "account_scoped": account_scoped}},
                 {"type": "turn_context", "payload": {"model": model}}]
        for key, age, tokens in events or [("response", -3600, 1_000_000)]:
            lines.append({"type": "token_usage_record", "timestamp": datetime.fromtimestamp(now + age, timezone.utc).isoformat(),
                          "payload": {"response_id": key, "usage": {"input_tokens": tokens}}})
        (root / name).write_text("\n".join(json.dumps(line) for line in lines) + "\n")
    return pages, data, log


def test_same_account_window_and_local_subset_with_shared_profile(scoped):
    pages, data, log = scoped
    log("correct.jsonl", events=[("recent", -3600, 1_000_000), ("week", -2 * 86400, 1_500_000),
                               ("future", 60, 9_000_000), ("too-old", -8 * 86400, 9_000_000)])
    log("other.jsonl", identity="other-account", events=[("wrong", -3600, 99_000_000)])
    log("unscoped.jsonl", account_scoped=False, events=[("unscoped", -3600, 99_000_000)])
    result = usage_estimate.collect(pages, data)
    assert result["table"] == [["", "Codex"], ["5시간", "$4.00"], ["주간", "$10.00"]]
    assert result["usage_value"] and result["side"]
    assert "이 PC 기록" in result["sub"] and "청구 아님" in result["sub"]
    # %가 바뀌어도 실제 토큰/구간이 같으면 환산액은 같다.
    pages[0]["bars"][0]["pct"] = 1
    assert usage_estimate.collect(pages, data)["table"] == result["table"]


def test_unmatched_account_unknown_model_and_empty_window_never_become_zero(scoped):
    pages, data, log = scoped
    log("wrong.jsonl", identity="other-account")
    assert usage_estimate.collect(pages, data)["table"][1][1] == "환산불가"
    log("old.jsonl", events=[("old", -8 * 86400, 1_000_000)])
    assert usage_estimate.collect(pages, data)["table"][1][1] == "기록없음"
    log("unknown.jsonl", model="unpriced-model")
    assert usage_estimate.collect(pages, data)["table"][1][1] == "환산불가"


def test_shared_snapshot_and_claude_without_account_provenance_are_unavailable(scoped):
    pages, data, log = scoped
    log("correct.jsonl")
    pages[0].pop("estimate_source")
    assert usage_estimate.collect(pages, data)["table"][1][1] == "환산불가"
    # provider id를 공유하는 다른 조직이 있으면 provider id만 일치하는 로그도 제외한다.
    pages[0]["estimate_source"] = {"provider": "codex", "as_of": usage_estimate.time.time()}
    data["codex"]["accounts"].append({"id": "other-org", "providerAccountId": "identity",
                                    "workspaceAccountId": "other-workspace"})
    assert usage_estimate.collect(pages, data)["table"][1][1] == "환산불가"
    log("workspace.jsonl", identity="workspace")
    assert usage_estimate.collect(pages, data)["table"][1][1] == "$4.00"
    pages[0]["estimate_source"] = {"provider": "claude", "as_of": usage_estimate.time.time()}
    assert usage_estimate.collect(pages, data)["table"][1][1] == "환산불가"


def test_stale_or_expired_limit_cannot_be_converted(scoped):
    pages, data, log = scoped
    log("correct.jsonl")
    pages[0]["estimate_source"]["as_of"] -= 601
    assert usage_estimate.collect(pages, data)["table"][1][1] == "환산불가"
    pages[0]["estimate_source"]["as_of"] = usage_estimate.time.time()
    pages[0]["bars"][0]["resets"] = usage_estimate.time.time() - 1
    assert usage_estimate.collect(pages, data)["table"][1][1] == "환산불가"


def test_finger_swipe_to_value_and_return(tmp_path):
    compiler = shutil.which("c++")
    if not compiler:
        pytest.skip("C++ compiler required")
    firmware = Path(__file__).resolve().parents[2] / "firmware"
    binary = tmp_path / "usage-swipe"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(firmware / "src"),
                    str(firmware / "tests/usage_swipe_test.cpp"), "-o", str(binary)],
                   check=True, capture_output=True, timeout=30)
    subprocess.run([str(binary)], check=True, capture_output=True, timeout=10)
