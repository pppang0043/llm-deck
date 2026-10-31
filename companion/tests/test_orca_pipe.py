"""Direct RPC (Windows pipe, Mac socket): fallback must never duplicate terminal input."""
import json
import os
import shutil
import socket
import tempfile
import threading
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
    monkeypatch.setattr(orca.sys, "platform", "win32")
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
    assert orca._runtime_pipe() is None  # Mac 은 unix 소켓만 쓴다
    (tmp_path / "orca-runtime.json").write_text(json.dumps({
        "authToken": "test-token", "transports": [{"kind": "unix", "endpoint": "/tmp/test.sock"}]
    }))
    assert orca._runtime_pipe() == ("/tmp/test.sock", "test-token")
    monkeypatch.setattr(orca.sys, "platform", "linux")
    assert orca._runtime_pipe() is None


def test_mac_reads_directly_and_sends_by_cli(monkeypatch):
    monkeypatch.setattr(orca.sys, "platform", "darwin")
    inst = orca.Orca({"enabled": False})
    inst.cli = "orca"
    asked = []
    monkeypatch.setattr(orca, "_pipe_call", lambda *a: (asked.append(a[0]), ("answered", {"direct": True}))[1])
    ran = []

    def cli(args, **kwargs):
        ran.append(args)
        return SimpleNamespace(stdout='{"ok":true,"result":{"cli":true}}')

    monkeypatch.setattr(orca.subprocess, "run", cli)
    assert inst._run("worktree", "ps") == {"direct": True}
    assert inst._run("terminal", "list") == {"direct": True}
    assert asked == ["worktree.ps", "terminal.list"] and not ran
    assert inst._run("terminal", "send", "--terminal", "x", "--text", "hi") == {"cli": True}
    assert asked == ["worktree.ps", "terminal.list"] and len(ran) == 1


def test_unix_socket_call(monkeypatch):
    # 짧은 주소가 필요하다 (Mac 의 소켓 경로는 104바이트까지)
    folder = tempfile.mkdtemp(dir="/tmp")
    path = os.path.join(folder, "o.sock")
    server = socket.socket(socket.AF_UNIX)
    server.bind(path)
    server.listen(1)
    seen = {}

    def serve():
        conn, _ = server.accept()
        with conn:
            request = json.loads(conn.makefile().readline())
            seen.update(request)
            conn.sendall(b'{"_keepalive":true}\n')
            conn.sendall(json.dumps({"id": request["id"], "ok": True, "result": {"terminals": []}}).encode() + b"\n")

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    monkeypatch.setattr(orca.sys, "platform", "darwin")
    monkeypatch.setattr(orca, "_runtime_pipe", lambda: (path, "test-token"))
    try:
        assert orca._pipe_call("terminal.list", {}, 2) == ("answered", {"terminals": []})
        assert seen["authToken"] == "test-token" and seen["method"] == "terminal.list"
        monkeypatch.setattr(orca, "_runtime_pipe", lambda: (path + ".gone", "test-token"))
        assert orca._pipe_call("terminal.list", {}, 1) == ("unsent", None)
    finally:
        thread.join(2)
        server.close()
        shutil.rmtree(folder)


def test_pipe_unavailable(monkeypatch):
    monkeypatch.setattr(orca.sys, "platform", "win32")
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
    monkeypatch.setattr(orca.sys, "platform", "win32")
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
