"""질문의 답을 이 PC 스피커로 읽어 준다 (운영체제에 들어 있는 음성, 인터넷 안 씀).

    Mac: say (한국어 목소리 Yuna). Windows: PowerShell 의 System.Speech (한국어 목소리가 있으면 그것).
PC 의 음량은 건드리지 않는다. 지금 PC 에 맞춰 둔 음량 그대로 나온다.
"""

import base64
import re
import subprocess
import sys
import threading

_NO_WINDOW = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0

# Windows: 글은 base64 로 넘긴다 (명령 줄·콘솔 글자 코드 때문에 한글이 깨지지 않게)
_PS_SCRIPT = (
    "Add-Type -AssemblyName System.Speech;"
    "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer;"
    "$v = $s.GetInstalledVoices() | Where-Object {{ $_.Enabled -and $_.VoiceInfo.Culture.Name -eq 'ko-KR' }} | Select-Object -First 1;"
    "if ($v) {{ $s.SelectVoice($v.VoiceInfo.Name) }};"
    "$s.Speak([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{text}')))"
)


def _clean(text: str) -> str:
    """읽기 좋게: 마크다운 기호를 빼고 줄바꿈은 쉼으로."""
    text = re.sub(r"[*#`_>|]", " ", text)
    text = re.sub(r"([^.!?:])\s*\n\s*", r"\1. ", text.strip())
    text = re.sub(r"\s*\n\s*", " ", text)
    return re.sub(r"\s{2,}", " ", text)


class Speaker:
    def __init__(self, cfg: dict):
        self.voice = cfg.get("voice", "Yuna")  # Mac 목소리 (say -v '?' 로 목록)
        self.proc: subprocess.Popen | None = None
        self.lock = threading.Lock()

    def speak(self, text: str) -> None:
        """앞에 읽던 것은 끊고 새로 읽기 시작한다 (기다리지 않는다)."""
        text = _clean(text)
        self.stop()
        if not text:
            return
        if sys.platform == "darwin":
            args = ["say", "-v", self.voice, text]
        elif sys.platform == "win32":
            encoded = base64.b64encode(text.encode("utf-8")).decode()
            args = ["powershell", "-NoProfile", "-NonInteractive", "-Command", _PS_SCRIPT.format(text=encoded)]
        else:
            print("[speech] 이 운영체제는 아직 읽어 주기를 하지 않습니다")
            return
        try:
            with self.lock:
                self.proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                             stderr=subprocess.DEVNULL, creationflags=_NO_WINDOW)
        except OSError as e:
            print(f"[speech] 읽어 주지 못했습니다: {e}")

    def stop(self) -> None:
        with self.lock:
            proc, self.proc = self.proc, None
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(2)
            except subprocess.TimeoutExpired:
                proc.kill()
