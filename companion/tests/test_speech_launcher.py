"""Windows 에서 PowerShell 대신 쓰는 것: 답 읽어 주기(speech.py, SAPI)와 파워쉘 버튼의 명령 프롬프트 대안(launcher.py)."""

import sys

import launcher
import speech


class FakeSapi:
    def __init__(self):
        self.said, self.stops = [], 0

    def speak(self, text):
        self.said.append(text)

    def stop(self):
        self.stops += 1


def test_sapi_text_escapes_and_prefers_korean_voice():
    assert speech._sapi_text("a < b & c") == '<voice optional="Language=412">a &lt; b &amp; c</voice>'


def test_windows_speaks_with_sapi_without_powershell(monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    spawned = []
    monkeypatch.setattr(speech.subprocess, "Popen", lambda *a, **k: spawned.append(a))
    fake = FakeSapi()
    monkeypatch.setattr(speech, "_Sapi", lambda: fake)
    s = speech.Speaker({})
    s.speak("**안녕**\n하세요")
    assert fake.said == ["안녕. 하세요"] and not spawned  # 읽던 것은 SAPI 가 버리고 새로 읽는다 (PURGE)
    s.speak("다음")
    s.stop()
    assert fake.said[-1] == "다음" and fake.stops == 2


def test_windows_powershell_fallback_only_when_allowed(monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setattr(speech, "_NO_WINDOW", 0)
    spawned = []

    class Proc:
        def __init__(self, args, **kw):
            spawned.append((args, kw.get("env")))

        def poll(self):
            return 0

    monkeypatch.setattr(speech.subprocess, "Popen", Proc)

    def broken():
        raise OSError("SAPI 없음")

    monkeypatch.setattr(speech, "_Sapi", broken)
    speech.Speaker({}).speak("답")
    assert not spawned  # 처음에는 PowerShell 을 띄우지 않는다
    s = speech.Speaker({"speak_powershell": True})
    s.speak("답")
    args, env = spawned[0]
    assert args[0] == "powershell" and "-ExecutionPolicy" not in args and env[speech._TEXT_ENV] == "답"
    s.speak("또")
    assert len(spawned) == 2  # SAPI 는 한 번만 시도한다


def test_powershell_button_falls_back_to_cmd(monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    opened = []

    def fake_open(win_name, mac_name, admin=False, cancelled=None):
        opened.append(win_name)
        return win_name == "cmd"

    monkeypatch.setattr(launcher.Launcher, "_open", staticmethod(fake_open))
    monkeypatch.setattr(launcher.threading, "Thread", lambda target, daemon: type("T", (), {"start": lambda self: target()})())
    app = launcher.Launcher()
    app.on_event({"launch": "powershell"})
    assert opened == ["powershell", "cmd"]
    assert app.pending_toast == "파워쉘 대신 명령 프롬프트를 열었습니다"


def test_declined_admin_prompt_is_not_asked_again(monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    opened = []

    def fake_open(win_name, mac_name, admin=False, cancelled=None):
        opened.append((win_name, admin))
        if cancelled is not None:
            cancelled[win_name] = True  # [아니요]
        return False

    monkeypatch.setattr(launcher.Launcher, "_open", staticmethod(fake_open))
    monkeypatch.setattr(launcher.threading, "Thread", lambda target, daemon: type("T", (), {"start": lambda self: target()})())
    app = launcher.Launcher()
    app.on_event({"launch": "powershell_admin"})
    assert opened == [("powershell", True)]
    assert "허용하지 않았습니다" in app.pending_toast
