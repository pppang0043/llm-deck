"""도우미 상태("helper")와 펌웨어 버전 비교."""

import version


def test_status_fields(monkeypatch):
    monkeypatch.setattr(version, "_version", "abc1234")
    assert version.status(1700000000.7, {}) == {"proto": version.PROTO, "ver": "abc1234", "at": 1700000000}
    out = version.status(0, {"codex": "로그인 만료", "api_cost": "x"})
    assert out["err"] == "codex: 로그인 만료 외 1건"


def test_short_error_one_line_and_short():
    err = version.short_error({"billing": "a\nb " + "z" * 100})
    assert "\n" not in err and len(err) <= 48


def test_peer_logs_mismatch_once():
    logs = []
    peer = version.Peer(logs.append)
    peer.on_event({"fw_proto": version.PROTO, "fw": "Oct 1"})
    assert not any("오래되었" in line for line in logs)
    peer.reset()
    logs.clear()
    peer.on_event({"fw_proto": version.PROTO - 1})
    peer.on_event({"fw_proto": version.PROTO - 1})
    assert sum("펌웨어를 올리세요" in line for line in logs) == 1


def test_peer_silent_device_is_old():
    logs = []
    peer = version.Peer(logs.append)
    for _ in range(version.SILENT_SENDS):
        peer.sent()
    assert any("proto 0" in line for line in logs)


def test_version_names_the_commit_not_a_long_tag(monkeypatch):
    """태그 이름(companion-windows-2026-10-27-13-g7ef83ab)이 칸 길이에서 잘려 커밋을 구별하지 못한 적이 있다."""
    calls = []

    class Done:
        returncode = 0
        stdout = "7ef83ab-dirty\n"

    def run(cmd, **kwargs):
        calls.append(cmd)
        return Done()

    monkeypatch.setattr(version, "_version", None)
    monkeypatch.setattr(version.subprocess, "run", run)
    assert version.version() == "7ef83ab-dirty"
    assert "--tags" not in calls[0] and calls[0][-2:] == ["--exclude", "*"]
