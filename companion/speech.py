"""질문의 답을 이 PC 스피커로 읽어 준다 (운영체제에 들어 있는 음성, 인터넷 안 씀).

    Mac: say (한국어 목소리 Yuna).
    Windows: Windows 음성(SAPI)을 이 도우미 안에서 바로 부른다 (한국어 목소리가 있으면 그것). 다른 프로그램을 띄우지 않는다.
             SAPI 를 못 쓸 때만, config.toml [assistant] speak_powershell = true 면 PowerShell 로 읽는다
             (PowerShell 을 띄우는 모양을 회사 보안 프로그램이 의심할 수 있어 처음에는 꺼 둔다).
PC 의 음량은 건드리지 않는다. 지금 PC 에 맞춰 둔 음량 그대로 나온다.
"""

import os
import queue
import re
import subprocess
import sys
import threading
from xml.sax.saxutils import escape

_NO_WINDOW = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0

# SAPI 의 SpVoice 부품과 ISpVoice::Speak (vtable 20번째, sapi.h 순서)
_SPVOICE = "{96749377-3391-11D2-9EE3-00C04F797396}"
_ISPVOICE = "{6C44DF74-72B9-4992-A1EC-EF996E0422D4}"
_SPEAK = 20
_SPF_ASYNC, _SPF_PURGE, _SPF_XML = 1, 2, 8

# PowerShell 로 읽을 때: 글은 환경 변수로 넘긴다 (명령 줄·콘솔 글자 코드 때문에 한글이 깨지지 않게).
# 예전에는 base64 로 감싸 명령 줄에 넣었는데, 감춘 글을 PowerShell 안에서 푸는 모양을 회사 보안 프로그램이 의심한다
_TEXT_ENV = "LLMDECK_SPEAK"
_PS_SCRIPT = (
    "Add-Type -AssemblyName System.Speech;"
    "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer;"
    "$v = $s.GetInstalledVoices() | Where-Object { $_.Enabled -and $_.VoiceInfo.Culture.Name -eq 'ko-KR' } | Select-Object -First 1;"
    "if ($v) { $s.SelectVoice($v.VoiceInfo.Name) };"
    f"$s.Speak($env:{_TEXT_ENV})"
)


def _clean(text: str) -> str:
    """읽기 좋게: 마크다운 기호를 빼고 줄바꿈은 쉼으로."""
    text = re.sub(r"[*#`_>|]", " ", text)
    text = re.sub(r"([^.!?:])\s*\n\s*", r"\1. ", text.strip())
    text = re.sub(r"\s*\n\s*", " ", text)
    return re.sub(r"\s{2,}", " ", text)


def _sapi_text(text: str) -> str:
    """한국어 목소리가 있으면 그것으로 (0x412 = 한국어). 없으면 Windows 기본 목소리"""
    return f'<voice optional="Language=412">{escape(text)}</voice>'


class _Sapi:
    """SAPI 목소리 하나를 스레드 하나가 맡아 읽기·끊기를 한다. 읽기는 기다리지 않는다 (SAPI 가 따로 읽는다)"""

    def __init__(self) -> None:
        self.jobs: queue.Queue = queue.Queue()
        self.ready = threading.Event()
        self.error: Exception | None = None
        threading.Thread(target=self._run, daemon=True, name="speech").start()
        if not self.ready.wait(5):
            raise OSError("Windows 음성이 답하지 않습니다")
        if self.error:
            raise OSError(f"Windows 음성을 쓸 수 없습니다: {self.error}")

    def _run(self) -> None:
        from ctypes import c_ulong, c_void_p, c_wchar_p

        import wincom

        try:
            wincom.init()
            voice = wincom.create(_SPVOICE, _ISPVOICE)
        except Exception as e:
            self.error = e
            self.ready.set()
            return
        self.ready.set()
        while True:
            text, flags = self.jobs.get()
            try:
                wincom.call(voice, _SPEAK, (c_wchar_p, c_ulong, c_void_p), text, flags, None)
            except OSError as e:
                print(f"[speech] 읽어 주지 못했습니다: {e}")

    def speak(self, text: str) -> None:
        self.jobs.put((_sapi_text(text), _SPF_ASYNC | _SPF_PURGE | _SPF_XML))

    def stop(self) -> None:
        self.jobs.put(("", _SPF_ASYNC | _SPF_PURGE))  # 읽던 것을 버린다


class Speaker:
    def __init__(self, cfg: dict):
        self.voice = cfg.get("voice", "Yuna")  # Mac 목소리 (say -v '?' 로 목록)
        self.allow_powershell = cfg.get("speak_powershell", False) is True
        self.proc: subprocess.Popen | None = None
        self.sapi: _Sapi | None = None
        self.sapi_failed = False
        self.lock = threading.Lock()

    def _windows_voice(self) -> "_Sapi | None":
        """처음 읽을 때 SAPI 를 준비한다. 안 되면 다시 시도하지 않는다 (PowerShell 로 넘기거나 읽지 않는다)"""
        if self.sapi is None and not self.sapi_failed:
            try:
                self.sapi = _Sapi()
            except Exception as e:
                self.sapi_failed = True
                print(f"[speech] {e}" + (" - PowerShell 로 읽습니다" if self.allow_powershell else
                                         " (config.toml [assistant] speak_powershell = true 면 PowerShell 로 읽습니다)"))
        return self.sapi

    def speak(self, text: str) -> None:
        """앞에 읽던 것은 끊고 새로 읽기 시작한다 (기다리지 않는다)."""
        text = _clean(text)
        self.stop()
        if not text:
            return
        env = None
        if sys.platform == "darwin":
            args = ["say", "-v", self.voice, text]
        elif sys.platform == "win32":
            sapi = self._windows_voice()
            if sapi:
                sapi.speak(text)
                return
            if not self.allow_powershell:
                return
            args = ["powershell", "-NoProfile", "-NonInteractive", "-Command", _PS_SCRIPT]
            env = dict(os.environ, **{_TEXT_ENV: text})
        else:
            print("[speech] 이 운영체제는 아직 읽어 주기를 하지 않습니다")
            return
        try:
            with self.lock:
                self.proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                             stderr=subprocess.DEVNULL, env=env, creationflags=_NO_WINDOW)
        except OSError as e:
            print(f"[speech] 읽어 주지 못했습니다: {e}")

    def stop(self) -> None:
        if self.sapi:
            self.sapi.stop()
        with self.lock:
            proc, self.proc = self.proc, None
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(2)
            except subprocess.TimeoutExpired:
                proc.kill()
