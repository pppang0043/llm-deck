import pytest

import orca
import typer


@pytest.fixture
def o(tmp_path, monkeypatch):
    monkeypatch.setattr(orca, "_screen_order", lambda: ({}, {}))
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
    o.sent_cmds.clear()
    o.send_key("abc", "erase")  # 입력칸을 여러 줄까지 비우기: 나눠 보내고 Enter 는 치지 않는다
    sent = [c[5] for c in o.sent_cmds]
    assert all(c[:5] == ("terminal", "send", "--terminal", "term_abc", "--text") for c in o.sent_cmds)
    assert sent == orca.erase_keys() and len(sent) > 1
    assert sent[0].count("\x1b[3~") == orca.ERASE_CHUNK and sent[-2].count("\x7f") == orca.ERASE_CHUNK
    assert sum(k.count("\x7f") for k in sent) == orca.ERASE_ABOVE
    assert "\r" not in "".join(sent)
    o.sent_cmds.clear()
    o.send_key("abc", "orchestration")  # 띄어쓰기로 끝나는 키워드: 글만 보내고 Enter 는 치지 않는다
    assert o.sent_cmds == [("terminal", "send", "--terminal", "term_abc", "--text", "/orchestration ")]
    assert o.pending_toast == "보냈습니다: /orchestration"
    o.sent_cmds.clear()
    o.send_key("abc", "clear")  # /clear 키워드는 지우기와 다르다: /clear 를 치고 Enter
    assert o.sent_cmds == [("terminal", "send", "--terminal", "term_abc", "--text", "/clear"),
                           ("terminal", "send", "--terminal", "term_abc", "--enter")]


def test_session_order_follows_orca_screen(o, monkeypatch):
    # 상태(작업 중·권한 요청 등)와 상관없이 ORCA 화면 순서대로: 사이드바의 프로젝트 순서, 그 안에서 탭 순서
    def term(n, repo, tab_id, agent=True):
        return {"handle": f"term_{n}", "tabId": tab_id, "leafId": "l", "worktreeId": f"{repo}::/w/{repo}",
                "worktreePath": f"/w/{repo}", "title": f"✳ 세션{n}", "connected": True,
                "agentIdentity": "claude" if agent else None}

    terminals = [
        term("aaaa1", "alpha", "ta1"),
        term("bbbb1", "beta", "tb1"),
        term("bbbb2", "beta", "tb2"),
        term("cccc1", "gamma", "tc1", agent=False),
        term("zzzz1", "zeta", "tz1"),  # ORCA 화면 상태에 없는 것은 뒤에
        {**term("eeee1", "eps", "te1"), "connected": False},  # 끊긴 것은 뺀다
    ]
    states = {"aaaa1": "working", "bbbb1": "working", "bbbb2": "permission"}
    ps = {"worktrees": [{"agents": [{"paneKey": f"{t['tabId']}:l", "mainAgent": {"state": states[t['handle'][5:]]}}
                                    for t in terminals if t["handle"][5:] in states]}]}

    def run(*args, **kw):
        return ps if args[:2] == ("worktree", "ps") else {"terminals": terminals}

    o._run = run
    # 사이드바: gamma, beta, alpha 순. beta 안의 탭은 tb2, tb1 순
    monkeypatch.setattr(orca, "_screen_order", lambda: ({"gamma": 0, "beta": 1, "alpha": 2}, {"tb2": 0, "tb1": 1}))
    state = o._read()
    assert [r["id"] for r in state["list"]] == ["cccc1", "bbbb2", "bbbb1", "aaaa1", "zzzz1"]
    assert state["list"][1]["s"] == "permission"
    assert state["list"][1]["t"] == "세션bbbb2"  # 앞의 ✳ 표시를 뗀다
    assert o.handles["bbbb2"] == "term_bbbb2"

    # 화면 상태를 못 읽으면 orca terminal list 순서 그대로
    monkeypatch.setattr(orca, "_screen_order", lambda: ({}, {}))
    assert [r["id"] for r in o._read()["list"]] == ["aaaa1", "bbbb1", "bbbb2", "cccc1", "zzzz1"]


def test_orchestration_workers_hidden(o, monkeypatch):
    # /orchestration 이 띄운 워커(parentPaneKey 가 있는 것)는 목록에서 뺀다
    terminals = [{"handle": f"term_{n}", "tabId": n, "leafId": "l", "worktreePath": "/w/a",
                  "title": n, "connected": True, "agentIdentity": "claude"} for n in ("main1", "work1")]
    ps = {"worktrees": [{"agents": [{"paneKey": "main1:l", "parentPaneKey": None, "state": "working"},
                                    {"paneKey": "work1:l", "parentPaneKey": "main1:l", "state": "working"}]}]}
    o._run = lambda *a, **k: ps if a[:2] == ("worktree", "ps") else {"terminals": terminals}
    monkeypatch.setattr(orca, "_screen_order", lambda: ({}, {}))
    assert [r["id"] for r in o._read()["list"]] == ["main1"]
    assert "work1" not in o.handles


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


@pytest.fixture
def two_tabs(o, monkeypatch):
    # 받아쓰기를 고른 세션(abc, 탭 t1)과 다른 에이전트의 세션(xyz, 탭 t2)
    o.handles = {"abc": "term_abc", "xyz": "term_xyz"}
    o.tabs = {"term_abc": "t1", "term_xyz": "t2"}
    o.state = {"ok": True, "list": [{"id": "abc", "t": "내 세션"}, {"id": "xyz", "t": "남의 세션"}]}
    o._run = lambda *a, **k: o.sent_cmds.append(a) or {}
    copied = []
    monkeypatch.setattr(orca, "_copy", lambda text: copied.append(text) or True)
    return copied


def test_send_text_when_session_showing(o, two_tabs, monkeypatch):
    monkeypatch.setattr(orca, "active_tab", lambda: "t1")
    assert o.send_text("term_abc", "안녕") is None
    assert o.sent_cmds == [("terminal", "send", "--terminal", "term_abc", "--text", "안녕")]
    assert two_tabs == []


def test_send_text_refused_after_other_session_took_screen(o, two_tabs, monkeypatch):
    # 받아 적는 동안 다른 세션이 자기 세션을 앞으로 띄웠다: 넣지 않고 글은 클립보드에 남긴다
    monkeypatch.setattr(orca, "active_tab", lambda: "t2")
    error = o.send_text("term_abc", "안녕")
    assert "남의 세션" in error and "클립보드" in error
    assert o.sent_cmds == []
    assert two_tabs == ["안녕"]


def test_send_text_waits_for_late_saved_tab(o, two_tabs, monkeypatch):
    # 막 바꾼 탭은 ORCA 가 1~1.5초 뒤에 저장한다: 한 번 더 읽어 보고 맞으면 넣는다
    seen = iter(["t2", "t1"])
    monkeypatch.setattr(orca, "active_tab", lambda: next(seen))
    assert o.send_text("term_abc", "안녕") is None
    assert o.sent_cmds


def test_send_text_when_screen_state_unreadable(o, two_tabs, monkeypatch):
    # 예전 ORCA 등으로 화면 상태를 못 읽으면 막지 않는다 (handle 로 그 세션에만 보낸다)
    monkeypatch.setattr(orca, "active_tab", lambda: None)
    assert o.send_text("term_abc", "안녕") is None


def test_send_key_esc_without_enter(o):
    # 팝업의 Esc 버튼: Esc 한 번만 보내고 Enter 는 치지 않는다
    o.handles = {"abc": "term_abc"}
    o.sent_cmds = []
    o._run = lambda *args, **kw: o.sent_cmds.append(args) or {}
    o.send_key("abc", "esc")
    assert o.sent_cmds == [("terminal", "send", "--terminal", "term_abc", "--text", "\x1b")]
    assert o.pending_toast == "보냈습니다: Esc"
