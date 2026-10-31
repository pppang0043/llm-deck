import copy
import json
import subprocess
import time
from types import SimpleNamespace

import pytest

from collectors import orca_usage


@pytest.fixture
def account_data():
    now = int(time.time() * 1000)
    return {
        "claude": {"accounts": [{"id": "a", "email": "a@example.test"},
                                 {"id": "b", "email": "b@example.test"}], "activeAccountId": "a"},
        "codex": {"accounts": [], "activeAccountId": None,
                  "systemDefault": {"email": "system@example.test"}},
        "rateLimits": {p: {"updatedAt": now, "session": {"usedPercent": 23, "resetsAt": now + 3600000},
                            "weekly": {"usedPercent": 67, "resetsAt": now + 86400000}}
                       for p in ("claude", "codex")},
    }


def test_units_and_selected_identity(account_data):
    pages = orca_usage.pages(account_data, {})
    page = pages["claude_code"]
    assert [b["pct"] for b in page["bars"]] == [23, 67]
    assert page["bars"][0]["resets"] == account_data["rateLimits"]["claude"]["session"]["resetsAt"] // 1000
    assert page["rows"][0] == ["계정", "a@example.test"]
    assert pages["codex"]["rows"][0][1] == "system@example.test"
    changed = copy.deepcopy(account_data)
    changed["claude"]["activeAccountId"] = "b"
    other = orca_usage.pages(changed, {})["claude_code"]
    assert other["name"] != page["name"]  # 그래프·알림에서 다른 계정과 섞이지 않는다
    assert other["rows"][0][1] == "b@example.test"


def test_unavailable_usage_does_not_invent_zero(account_data):
    account_data["rateLimits"]["claude"] = {"error": "expired"}
    page = orca_usage.pages(account_data, {})["claude_code"]
    assert page["headline"] == "-" and "bars" not in page
    assert "codex" not in orca_usage.pages(account_data, {"codex": {"enabled": False}})


def test_wsl_target_is_not_host_account(account_data):
    account_data["rateLimits"]["claudeTarget"] = {"runtime": "wsl"}
    assert "bars" not in orca_usage.pages(account_data, {})["claude_code"]


def test_cached_error_keeps_timestamp(account_data):
    account_data["rateLimits"]["claude"]["error"] = "offline"
    page = orca_usage.pages(account_data, {})["claude_code"]
    assert page["bars"][0]["pct"] == 23
    assert "마지막" in page["sub"]


def test_read_uses_local_account_list(monkeypatch, account_data):
    monkeypatch.setattr(orca_usage.orca, "find_orca", lambda: "/local/orca")
    def run(args, **kwargs):
        assert args == ["/local/orca", "account", "list", "--json"]
        assert kwargs["timeout"] == 10
        return SimpleNamespace(returncode=0, stdout=json.dumps({"ok": True, "result": account_data}))
    monkeypatch.setattr(orca_usage.subprocess, "run", run)
    assert orca_usage.read() == account_data


@pytest.mark.parametrize("failure", ["invalid", "failed", "timeout"])
def test_read_failure(monkeypatch, failure):
    monkeypatch.setattr(orca_usage.orca, "find_orca", lambda: "/local/orca")
    def run(*args, **kwargs):
        if failure == "timeout":
            raise subprocess.TimeoutExpired("orca", 10)
        return SimpleNamespace(returncode=int(failure == "failed"), stdout="invalid")
    monkeypatch.setattr(orca_usage.subprocess, "run", run)
    assert orca_usage.read() is None


@pytest.mark.parametrize("orca_state", ["ok", "expired", "offline", "disabled"])
def test_collection_preserves_originals_and_adds_codex(monkeypatch, account_data, orca_state):
    import main
    original = {"as_of": time.time(), "windows": [["5h", 45, None]]}
    class Shared:
        def merge(self, local):
            assert local == {"claude_code": original, "codex": None}
            return {"codex": {"as_of": time.time(), "windows": [["5h", 99, None]]}}
    monkeypatch.setattr(main, "_sync", Shared())
    def read():
        assert orca_state != "disabled"
        return None if orca_state == "offline" else account_data
    monkeypatch.setattr(orca_usage, "read", read)
    monkeypatch.setattr(main.LIMITS[0][1], "snapshot", lambda cfg: original)
    monkeypatch.setattr(main.LIMITS[1][1], "snapshot", lambda cfg: None)
    if orca_state == "expired":
        account_data["rateLimits"]["codex"] = {"error": "expired"}
    pages = main.collect_limits({"orca": {"usage": orca_state != "disabled"}})
    assert [p["name"] for p in pages[:2]] == ["Claude Code", "Codex"]
    assert [p["bars"][0]["pct"] for p in pages[:2]] == [45, 99]
    if orca_state in ("offline", "disabled"):
        assert len(pages) == 2
    else:
        assert len(pages) == 3 and pages[2]["name"].startswith("ORCA Codex ")
        if orca_state == "expired":
            assert pages[2]["headline"] == "-" and "bars" not in pages[2]
        else:
            assert pages[2]["bars"][0]["pct"] == 23
