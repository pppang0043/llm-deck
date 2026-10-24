import io
import urllib.error

from collectors import server


def _setup(monkeypatch, tmp_path, now):
    monkeypatch.setattr(server, "STATE", tmp_path / "server-usage.json")
    monkeypatch.setattr(server, "_cache", {})
    monkeypatch.setattr(server, "_backoff", {})
    monkeypatch.setattr(server, "_quiet", set())
    monkeypatch.setattr(server.time, "time", lambda: now[0])


def _too_many(retry_after=None):
    headers = {"Retry-After": retry_after} if retry_after else {}
    return urllib.error.HTTPError("https://x", 429, "Too Many Requests", headers, io.BytesIO())


def test_restart_does_not_ask_again_right_away(monkeypatch, tmp_path):
    now = [1000.0]
    _setup(monkeypatch, tmp_path, now)
    calls = []
    fetch = lambda: calls.append(1) or {"windows": []}
    server._cached("claude", fetch)
    monkeypatch.setattr(server, "_cache", {})  # 도우미 재시작
    now[0] += 30
    server._cached("claude", fetch)
    assert len(calls) == 1
    now[0] += server.INTERVAL
    server._cached("claude", fetch)
    assert len(calls) == 2


def test_too_many_requests_backs_off_and_keeps_last_value(monkeypatch, tmp_path):
    now = [1000.0]
    _setup(monkeypatch, tmp_path, now)
    good = {"windows": [["5h", 10.0, None]]}
    assert server._cached("claude", lambda: good) == good

    def busy():
        raise _too_many()

    waits = []
    for _ in range(4):
        now[0] = server._cache["claude"][0]
        assert server._cached("claude", busy) == good
        waits.append(server._cache["claude"][0] - now[0])
    assert waits == [600, 1200, 2400, 3600]

    now[0] = server._cache["claude"][0]
    assert server._cached("claude", lambda: good) == good
    assert server._cache["claude"][0] - now[0] == server.INTERVAL


def test_too_many_requests_honours_retry_after(monkeypatch, tmp_path):
    now = [1000.0]
    _setup(monkeypatch, tmp_path, now)

    def busy():
        raise _too_many("1800")

    server._cached("claude", busy)
    assert server._cache["claude"][0] - now[0] == 1800
