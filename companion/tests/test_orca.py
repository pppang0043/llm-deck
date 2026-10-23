import pytest

import orca
import typer


@pytest.fixture
def o(tmp_path, monkeypatch):
    monkeypatch.setattr(orca, "RECENT_FILE", tmp_path / "recent.json")
    monkeypatch.setattr(orca, "_custom_titles", lambda: {})
    monkeypatch.setattr(orca.time, "sleep", lambda s: None)
    inst = orca.Orca({"enabled": False})  # 읽는 스레드는 띄우지 않는다
    inst.sent_cmds = []
    return inst


def fake_session(o, state):
    """_read 가 늘 세션 하나(abc, 상태 state["s"])를 돌려주고, _run 은 보낸 것을 적기만 한다."""
    def read():
        o.handles = {"abc": "term_abc"}
        return {"ok": True, "list": [{"id": "abc", "t": "세션", "p": "proj", "s": state["s"]}]}

    o._read = read
    o._run = lambda *a, **k: o.sent_cmds.append(a) or {}


@pytest.mark.parametrize("reply, state, expected", [
    ("allow", "permission", [("terminal", "send", "--terminal", "term_abc", "--text", "1")]),
    ("deny", "permission", [("terminal", "send", "--terminal", "term_abc", "--text", "\x1b")]),
    ("continue", "waiting", [("terminal", "send", "--terminal", "term_abc", "--text", "계속"),
                             ("terminal", "send", "--terminal", "term_abc", "--enter")]),
    ("continue", "done", [("terminal", "send", "--terminal", "term_abc", "--text", "계속"),
                          ("terminal", "send", "--terminal", "term_abc", "--enter")]),
])
def test_reply_whitelist(o, reply, state, expected):
    fake_session(o, {"s": state})
    o.reply("abc", reply)
    assert o.sent_cmds == expected
    assert "보냈습니다" in o.pending_toast


@pytest.mark.parametrize("reply, state", [
    ("allow", "working"), ("deny", "done"), ("continue", "permission"), ("allow", "shell"),
])
def test_reply_rechecks_state(o, reply, state):
    fake_session(o, {"s": state})
    o.state = {"ok": True, "list": [{"id": "abc", "t": "세션", "p": "proj", "s": "permission"}]}  # 늦은 목록
    o.reply("abc", reply)
    assert o.sent_cmds == []
    assert "상태가 바뀌어" in o.pending_toast


@pytest.mark.parametrize("name", ["rm -rf", "yes", "", "ALLOW", "1"])
def test_reply_unknown_rejected(o, name):
    fake_session(o, {"s": "permission"})
    o.reply("abc", name)
    assert o.sent_cmds == []
    assert "모르는 답" in o.pending_toast


def test_reply_unknown_session(o):
    fake_session(o, {"s": "permission"})
    o.reply("zzz", "allow")
    assert o.sent_cmds == []
    assert "찾지 못했습니다" in o.pending_toast


def test_send_key_keyword_and_unknown(o):
    fake_session(o, {"s": "waiting"})
    o.handles = {"abc": "term_abc"}
    o.send_key("abc", "push")
    assert o.sent_cmds == [("terminal", "send", "--terminal", "term_abc", "--text", typer.KEYWORDS["push"]),
                           ("terminal", "send", "--terminal", "term_abc", "--enter")]
    o.sent_cmds.clear()
    o.send_key("abc", "rm -rf /")
    assert o.sent_cmds == [] and "모르는 키워드" in o.pending_toast
    o.send_key("abc", "tab_enter")
    assert o.sent_cmds == [("terminal", "send", "--terminal", "term_abc", "--text", "\t"),
                           ("terminal", "send", "--terminal", "term_abc", "--enter")]


def test_session_sort_order(o):
    def term(n, wt, agent=True):
        return {"handle": f"term_{n}", "tabId": f"t{n}", "leafId": "l", "worktreePath": f"/w/{wt}",
                "title": f"✳ 세션{n}", "connected": True, "agentIdentity": "claude" if agent else None}

    terminals = [
        term("aaaa1", "alpha"),               # 작업 중
        term("bbbb1", "beta"),                # 작업 중
        term("bbbb2", "beta"),                # 권한 요청
        term("cccc1", "gamma", agent=False),  # 그냥 터미널
        term("dddd1", "delta"),               # 입력 기다림
        {**term("eeee1", "eps"), "connected": False},  # 끊긴 것은 뺀다
    ]
    states = {"aaaa1": "working", "bbbb1": "working", "bbbb2": "permission", "dddd1": "waiting"}
    ps = {"worktrees": [{"agents": [{"paneKey": f"t{n}:l", "mainAgent": {"state": s}} for n, s in states.items()]}]}

    def run(*args, **kw):
        return ps if args[:2] == ("worktree", "ps") else {"terminals": terminals}

    o._run = run
    state = o._read()
    order = [(r["p"], r["s"]) for r in state["list"]]
    # 손댈 일이 있는 프로젝트(beta: 권한, delta: 입력)가 앞, 그냥 터미널과 작업 중만 있는 프로젝트는 뒤
    assert order == [("beta", "permission"), ("beta", "working"), ("delta", "waiting"),
                     ("gamma", "shell"), ("alpha", "working")]
    assert state["list"][0]["t"] == "세션bbbb2"  # 앞의 ✳ 표시를 뗀다
    assert o.handles["bbbb2"] == "term_bbbb2"


def test_read_when_cli_missing(o):
    o._run = lambda *a, **k: None
    assert o._read() == {"ok": False, "list": []}


def test_fix_slash():
    cmds = ["commit", "clear", "skill-creator"]
    aliases = orca.SLASH_ALIASES
    assert orca.fix_slash("슬래시 커밋", cmds, aliases) == "/commit"
    assert orca.fix_slash("/ clear.", cmds, aliases) == "/clear"
    assert orca.fix_slash("그냥 문장", cmds, aliases) == "그냥 문장"


@pytest.fixture
def no_orca(monkeypatch):
    """PATH·설치 자리·실행 중인 ORCA 어디에도 orca 가 없는 PC."""
    monkeypatch.delenv("ORCA_CLI_BIN_DIR", raising=False)
    monkeypatch.setattr(orca.shutil, "which", lambda name: None)
    monkeypatch.setattr(orca, "EXTRA_PATHS", [])
    monkeypatch.setattr(orca.sys, "platform", "linux")


def test_find_orca_prefers_orca_bin_dir(tmp_path, monkeypatch, no_orca):
    (tmp_path / "orca").write_text("")
    monkeypatch.setenv("ORCA_CLI_BIN_DIR", str(tmp_path))
    monkeypatch.setattr(orca.shutil, "which", lambda name: "/elsewhere/orca")
    assert orca.find_orca() == str(tmp_path / "orca")


def test_find_orca_uses_shim_only_as_last_resort(tmp_path, monkeypatch, no_orca):
    shim = tmp_path / "shim" / "orca.cmd"  # install.ps1 이 까는 것. 옆에 orca.exe 가 없다
    shim.parent.mkdir()
    shim.write_text("")
    real = tmp_path / "Orca" / "orca.exe"
    real.parent.mkdir()
    real.write_text("")
    monkeypatch.setattr(orca.shutil, "which", lambda name: str(shim))
    monkeypatch.setattr(orca, "EXTRA_PATHS", [str(real)])
    assert orca.find_orca() == str(real)
    real.unlink()
    assert orca.find_orca() == str(shim)


def test_find_orca_missing(no_orca):
    assert orca.find_orca() is None
