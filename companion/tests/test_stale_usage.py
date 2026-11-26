"""비정상 종료 뒤 남은 [orca] usage = false 복구."""

import os

import stale_usage


def test_clear_orca_usage_false_removes_only_orca_line(tmp_path):
    path = tmp_path / "config.toml"
    path.write_text(
        "[device]\nlink = \"auto\"\n\n[orca]\n# keep me\nusage = false\nenabled = true\n\n"
        "[assistant]\nusage = false\n",
        encoding="utf-8",
    )
    assert stale_usage.clear_orca_usage_false(path)
    text = path.read_text(encoding="utf-8")
    assert "usage = false" not in text.split("[assistant]")[0]
    assert "[assistant]\nusage = false" in text
    assert "enabled = true" in text and "# keep me" in text
    assert not stale_usage.clear_orca_usage_false(path)


def test_recover_stale_orca_usage_only_after_crash(tmp_path, capsys):
    path = tmp_path / "config.toml"
    path.write_text("[orca]\nusage = false\n", encoding="utf-8")
    cfg = {"orca": {"usage": False, "enabled": True}}
    assert stale_usage.recover_stale_orca_usage(path, cfg, crashed=False)["orca"]["usage"] is False
    assert path.read_text(encoding="utf-8").count("usage = false") == 1
    got = stale_usage.recover_stale_orca_usage(path, cfg, crashed=True)
    assert "usage" not in got["orca"] and got["orca"]["enabled"] is True
    assert "usage = false" not in path.read_text(encoding="utf-8")
    assert "usage = false 를 지웠습니다" in capsys.readouterr().out


def test_previous_helper_crashed_detects_dead_pid(tmp_path, monkeypatch):
    pid_file = tmp_path / "companion.pid"
    monkeypatch.setattr(stale_usage, "PID_FILE", pid_file)
    assert not stale_usage.previous_helper_crashed()

    pid_file.write_text("424242", encoding="utf-8")
    monkeypatch.setattr(stale_usage, "pid_alive", lambda pid: False)
    assert stale_usage.previous_helper_crashed()
    assert not pid_file.exists()

    pid_file.write_text(str(os.getpid()), encoding="utf-8")
    assert not stale_usage.previous_helper_crashed()

    pid_file.write_text("424243", encoding="utf-8")
    monkeypatch.setattr(stale_usage, "pid_alive", lambda pid: True)
    assert not stale_usage.previous_helper_crashed()
    assert pid_file.exists()


def test_mark_and_clear_pid(tmp_path, monkeypatch):
    pid_file = tmp_path / "companion.pid"
    monkeypatch.setattr(stale_usage, "PID_FILE", pid_file)
    stale_usage.mark_running()
    assert pid_file.read_text(encoding="utf-8") == str(os.getpid())
    stale_usage.clear_pid()
    assert not pid_file.exists()


def test_claim_device_retries_after_orphan_reclaim(monkeypatch):
    import main

    binds = [None, object()]

    def try_bind():
        return binds.pop(0)

    monkeypatch.setattr(main, "_try_bind_instance", try_bind)
    monkeypatch.setattr(main, "_reclaim_orphan_helpers", lambda: True)
    monkeypatch.setattr(main.time, "sleep", lambda s: None)
    monkeypatch.delenv("LLMDECK_RESTARTED", raising=False)
    main._instance_lock = None
    assert main.claim_device() is True
    assert main._instance_lock is not None
