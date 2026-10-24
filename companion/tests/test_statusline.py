"""Claude Code 상태줄 설치 (statusline.py). Windows 는 PowerShell 없이 이 PC 의 Python 으로 돈다."""

import json
import subprocess
import sys

import pytest

import statusline


@pytest.fixture
def win(tmp_path, monkeypatch):
    data = tmp_path / ".llm-deck"
    data.mkdir()
    monkeypatch.setattr(statusline, "WINDOWS", True)
    monkeypatch.setattr(statusline, "DATA_DIR", data)
    monkeypatch.setattr(statusline, "SCRIPT", data / "statusline.py")
    monkeypatch.setattr(statusline, "LEGACY_SCRIPT", data / "statusline.ps1")
    monkeypatch.setattr(statusline, "NEXT", data / "statusline-next")
    monkeypatch.setattr(statusline, "NEXT_CMD", data / "statusline-next.cmd")
    settings = tmp_path / ".claude" / "settings.json"
    monkeypatch.setattr(statusline, "SETTINGS", settings)
    monkeypatch.setattr(statusline, "BACKUP", settings.with_name("settings.json.llm-deck-backup"))
    monkeypatch.setattr(statusline.sys, "executable", r"C:\Program Files\Python312\pythonw.exe")
    return data, settings


def write_settings(settings, command):
    settings.parent.mkdir(exist_ok=True)
    settings.write_text(json.dumps({"statusLine": {"type": "command", "command": command}}), encoding="utf-8")


def command(settings):
    return json.loads(settings.read_text(encoding="utf-8"))["statusLine"]["command"]


def test_windows_command_uses_installed_python_not_powershell(win, monkeypatch):
    monkeypatch.setattr(statusline.os.path, "exists", lambda path: True)  # pythonw.exe 옆의 python.exe
    cmd = statusline._command()
    assert "powershell" not in cmd.lower() and "Bypass" not in cmd
    assert cmd.startswith('"C:/Program Files/Python312/python.exe" "') and cmd.endswith('statusline.py"')


def test_fresh_install_chains_the_users_command(win):
    data, settings = win
    write_settings(settings, "my-status.cmd")
    statusline.install()
    assert command(settings) == statusline._command()
    assert (data / "statusline-next").read_text(encoding="utf-8") == "my-status.cmd\n"
    assert (data / "statusline.py").exists()


def test_legacy_powershell_install_is_replaced_not_chained(win):
    data, settings = win
    legacy = f'powershell -NoProfile -ExecutionPolicy Bypass -File "{data / "statusline.ps1"}"'
    write_settings(settings, legacy)
    (data / "statusline.ps1").write_text("old", encoding="utf-8")
    (data / "statusline-next").write_text("my-status.cmd\n", encoding="utf-8")
    statusline.install()
    assert command(settings) == statusline._command()
    assert (data / "statusline-next").read_text(encoding="utf-8") == "my-status.cmd\n"  # 원래 명령은 그대로
    assert not (data / "statusline.ps1").exists()
    statusline.uninstall()
    assert command(settings) == "my-status.cmd"


def test_windows_script_saves_rate_limits(tmp_path):
    script = tmp_path / "statusline.py"
    script.write_text(statusline.SCRIPT_BODY_WINDOWS, encoding="utf-8")
    home = tmp_path / "home"
    (home / ".llm-deck").mkdir(parents=True)
    payload = '{"rate_limits": {"five_hour": 12}}'
    env = {"HOME": str(home), "USERPROFILE": str(home), "PATH": "/usr/bin:/bin"}
    subprocess.run([sys.executable, str(script)], input=payload.encode(), env=env, check=True)
    assert (home / ".llm-deck" / "claude-statusline.json").read_text() == payload
