import json
import time
from types import SimpleNamespace

import pytest

from collectors import orca_usage


@pytest.fixture
def data():
    now = int(time.time() * 1000)
    win = lambda pct, after: {"usedPercent": pct, "resetsAt": now + after}
    return {
        "claude": {"accounts": [{"id": "a", "email": "me@x.test", "organizationName": "me@x.test's Organization"},
                                {"id": "b", "email": "me@x.test", "organizationName": "Team"},
                                {"id": "c", "email": "other@x.test"}],
                   "activeAccountIdsByRuntime": {"host": "a"}, "activeAccountId": "a"},
        "codex": {"accounts": [{"id": "d", "email": "side@x.test"}], "activeAccountId": None,
                  "systemDefault": {"email": "pc@x.test"}},
        "rateLimits": {
            "claude": {"updatedAt": now, "session": win(53, 3600000), "weekly": win(63, 86400000)},
            "codex": {"updatedAt": now, "session": win(10, 3600000), "weekly": win(100, 86400000)},
            "inactiveClaudeAccounts": [
                {"accountId": "b", "rateLimits": {"session": None, "weekly": None, "error": "stale"}},
                {"accountId": "c", "rateLimits": {"updatedAt": now - 2 * 86400000,
                                                  "session": win(40, -1000), "weekly": win(20, 86400000)}}],
            "inactiveCodexAccounts": [{"accountId": "d", "rateLimits": {"updatedAt": now,
                                                                       "session": win(5, 1), "weekly": win(51, 1)}}],
        },
    }


def test_main_pages_follow_selected_account(data):
    main, _ = orca_usage.pages(data, {})
    assert main["claude_code"]["name"] == "Claude me 개인"
    assert [b["pct"] for b in main["claude_code"]["bars"]] == [53, 63]
    # Codex 는 ORCA 에서 고른 계정이 없으면 PC 기본 로그인이다
    assert main["codex"]["name"] == "Codex pc"
    data["claude"]["activeAccountIdsByRuntime"]["host"] = "b"
    assert orca_usage.pages(data, {})[0]["claude_code"]["name"] == "Claude me Team"


def test_account_pages_list_every_account(data):
    _, extra = orca_usage.pages(data, {})
    claude, codex = extra
    assert claude["name"] == "Claude 계정"
    assert claude["rows"][1:] == [["me 개인 *", "53% / 63%"], ["me Team", "값 없음"], ["other", "0% / 20% · 2일 전"]]
    assert codex["rows"][1:] == [["pc *", "10% / 100%"], ["side", "5% / 51%"]]
    assert all("bars" not in page for page in extra)  # 알림과 기록을 만들지 않는다


def test_disabled_and_other_runtime_are_skipped(data):
    main, extra = orca_usage.pages(data, {"codex": {"enabled": False}})
    assert list(main) == ["claude_code"] and len(extra) == 1
    data["rateLimits"]["claudeTarget"] = {"runtime": "wsl"}
    main, extra = orca_usage.pages(data, {})
    assert list(main) == ["codex"]


def test_read_parses_cli_and_caches(monkeypatch, data):
    calls = []

    def run(*args, **kwargs):
        calls.append(args)
        return SimpleNamespace(returncode=0, stdout=json.dumps({"ok": True, "result": data}))

    monkeypatch.setattr(orca_usage, "_cache", (0.0, None))
    monkeypatch.setattr(orca_usage.time, "monotonic", lambda: 1000.0)
    monkeypatch.setattr(orca_usage.orca, "find_orca", lambda: "/local/orca")
    monkeypatch.setattr(orca_usage.subprocess, "run", run)
    assert orca_usage.read() == data
    assert orca_usage.read() == data
    assert len(calls) == 1


def test_read_failure_is_none(monkeypatch):
    monkeypatch.setattr(orca_usage, "_cache", (0.0, None))
    monkeypatch.setattr(orca_usage.time, "monotonic", lambda: 1000.0)
    monkeypatch.setattr(orca_usage.orca, "find_orca", lambda: "/local/orca")
    monkeypatch.setattr(orca_usage.subprocess, "run",
                        lambda *a, **k: SimpleNamespace(returncode=1, stdout=""))
    assert orca_usage.read() is None


@pytest.mark.parametrize("orca_state", ["ok", "expired", "offline", "disabled"])
def test_collection_uses_orca_or_falls_back(monkeypatch, data, orca_state):
    import main
    original = {"as_of": time.time(), "windows": [["5h", 45, None]]}

    class Shared:
        def merge(self, local):
            return {"codex": {"as_of": time.time(), "windows": [["5h", 99, None]]}}
    monkeypatch.setattr(main, "_sync", Shared())

    def read():
        assert orca_state != "disabled"
        return None if orca_state == "offline" else data
    monkeypatch.setattr(orca_usage, "read", read)
    monkeypatch.setattr(main.LIMITS[0][1], "snapshot", lambda cfg: original)
    monkeypatch.setattr(main.LIMITS[1][1], "snapshot", lambda cfg: None)
    if orca_state == "expired":
        data["rateLimits"]["codex"] = {"error": "expired"}
    pages = main.collect_limits({"orca": {"usage": orca_state != "disabled"}})
    assert len(pages) == 2  # 본 페이지 수는 그대로라 usage_N 번호가 바뀌지 않는다
    if orca_state in ("offline", "disabled"):
        assert [p["name"] for p in pages] == ["Claude Code", "Codex"]
        assert [p["bars"][0]["pct"] for p in pages] == [45, 99]
        assert main._orca_accounts == []
    else:
        assert [p["name"] for p in pages] == ["Claude me 개인", "Codex pc"]
        assert pages[0]["bars"][0]["pct"] == 53
        if orca_state == "expired":
            assert pages[1]["headline"] == "-" and "bars" not in pages[1]
        assert [p["name"] for p in main._orca_accounts] == ["Claude 계정", "Codex 계정"]
