"""Windows direct RPC: fallback must never duplicate terminal input."""
import json
from types import SimpleNamespace

import pytest

import orca


@pytest.mark.parametrize("args,method,params", [
    (("worktree", "ps"), "worktree.ps", {}),
    (("terminal", "list"), "terminal.list", {"includeVisualLayouts": False}),
    (("terminal", "switch", "--terminal", "term_x"), "terminal.focus",
     {"terminal": "term_x", "navigation": "host"}),
    (("terminal", "send", "--terminal", "term_x", "--text", "한글\n&%"), "terminal.send",
     {"terminal": "term_x", "text": "한글\n&%", "enter": False, "interrupt": False, "client": orca.CLI_CLIENT}),
    (("terminal", "send", "--terminal", "term_x", "--enter"), "terminal.send",
     {"terminal": "term_x", "enter": True, "interrupt": False, "client": orca.CLI_CLIENT}),
])
def test_request_matches_cli(args, method, params):
    assert orca._rpc_request(args) == (method, params)


def test_combined_text_enter_uses_cli():
    assert orca._rpc_request(("terminal", "send", "--terminal", "x", "--text", "y", "--enter")) is None


@pytest.mark.parametrize("status,send,fallback", [
    ("unsent", False, True), ("unsent", True, True),
    ("lost", False, True), ("lost", True, False),
    ("answered", False, False), ("answered", True, False),
])
def test_fallback_and_no_duplicate_input(monkeypatch, status, send, fallback):
    inst = orca.Orca({"enabled": False})
    inst.cli = "orca.exe"
    monkeypatch.setattr(orca, "_pipe_call", lambda *args: (status, None))
    calls = []

    def cli(*args, **kwargs):
        calls.append((args, kwargs))
        return SimpleNamespace(stdout='{"ok":true,"result":{"cli":true}}')

    monkeypatch.setattr(orca.subprocess, "run", cli)
    args = ("terminal", "send", "--terminal", "x", "--text", "hello") if send else ("terminal", "list")
    assert inst._run(*args) == ({"cli": True} if fallback else None)
    assert len(calls) == int(fallback)
    if fallback:
        assert calls[0][1]["timeout"] == 30


def test_runtime_metadata(tmp_path, monkeypatch):
    monkeypatch.setattr(orca.sys, "platform", "win32")
    monkeypatch.setenv("ORCA_USER_DATA_PATH", str(tmp_path))
    (tmp_path / "orca-runtime.json").write_text(json.dumps({
        "authToken": "test-token", "transports": [{"kind": "named-pipe", "endpoint": "test-pipe"}]
    }))
    assert orca._runtime_pipe() == ("test-pipe", "test-token")
    monkeypatch.setattr(orca.sys, "platform", "darwin")
    assert orca._runtime_pipe() is None


def test_pipe_unavailable(monkeypatch):
    monkeypatch.setattr(orca, "_runtime_pipe", lambda: ("missing", "test"))

    def unavailable(*a, **kw):
        raise OSError("missing")

    monkeypatch.setattr("builtins.open", unavailable)
    assert orca._pipe_call("terminal.list", {}, 1) == ("unsent", None)


@pytest.mark.parametrize("reply,expected", [
    ({"ok": True, "result": {"terminals": []}}, ("answered", {"terminals": []})),
    ({"ok": False, "error": {"message": "terminal_handle_stale"}}, ("answered", None)),
    (None, ("lost", None)),
])
def test_pipe_framing_and_error(monkeypatch, reply, expected):
    class Pipe:
        def __init__(self):
            self.written = b""
            self.chunks = None
            self.closed = False

        def write(self, data):
            n = min(7, len(data))  # raw writes can be short
            self.written += data[:n]
            return n

        def read(self, n):
            if self.chunks is None:
                request = json.loads(self.written)
                assert request["authToken"] == "test-token"
                response = b'\n{"_keepalive":true}\n'
                if reply:
                    response += (json.dumps({"id": request["id"], **reply}) + '\n').encode()
                self.chunks = [response[:13], response[13:], b""]
            return self.chunks.pop(0)

        def close(self):
            self.closed = True

    pipe = Pipe()
    monkeypatch.setattr(orca, "_runtime_pipe", lambda: ("test-pipe", "test-token"))
    monkeypatch.setattr("builtins.open", lambda *a, **kw: pipe)
    assert orca._pipe_call("terminal.list", {}, 1) == expected
    assert pipe.closed


def test_windows_which_rejects_this_module(tmp_path, monkeypatch):
    exe = tmp_path / "orca.exe"
    exe.touch()
    monkeypatch.setattr(orca.sys, "platform", "win32")
    monkeypatch.delenv("ORCA_CLI_BIN_DIR", raising=False)
    monkeypatch.delenv("NoDefaultCurrentDirectoryInExePath", raising=False)
    monkeypatch.setattr(orca.shutil, "which", lambda name: r".\orca.PY")
    monkeypatch.setattr(orca, "EXTRA_PATHS", [str(exe)])
    assert orca.find_orca() == str(exe)


def test_active_session_and_clear(monkeypatch):
    inst = orca.Orca({"enabled": False})
    monkeypatch.setattr(orca, "_screen_order", lambda: ({}, {}))
    monkeypatch.setattr(orca, "_custom_titles", lambda: {})
    inst._run = lambda *args: ({"worktrees": []} if args[0] == "worktree" else {
        "terminals": [{"handle": "term_abc", "tabId": "tab1", "connected": True}]})
    monkeypatch.setattr(orca, "active_tab", lambda: "tab1")
    assert inst._read()["a"] == "abc"
    monkeypatch.setattr(orca, "active_tab", lambda: None)
    assert inst._read()["a"] == ""
