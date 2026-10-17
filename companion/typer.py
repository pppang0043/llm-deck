"""기기의 키워드 버튼({"type": 이름})을 누르면 이 PC 에서 지금 커서가 있는 곳에 정해 둔 글을 입력한다.

여기 적어 둔 글만 입력한다 (기기나 다른 사람이 아무 글·명령이나 치게 하지 못하게). Enter 는 누르지 않는다.
  - Windows: 글자를 하나씩 키보드 입력으로 보낸다 (SendInput 의 유니코드 입력. 한글도 한/영 상태와 상관없이 들어간다).
    관리자 권한으로 띄운 창에는 입력되지 않는다 (Windows 가 막는다).
  - Mac: 클립보드에 넣고 Cmd+V 로 붙여 넣은 뒤 클립보드를 되돌린다.
    처음 한 번 시스템 설정 > 개인정보 보호 및 보안 > 손쉬운 사용 에서 "LLM Deck" 을 허용해야 한다.
결과는 기기 화면에 짧은 알림으로 보낸다 (pending_toast).
"""

import subprocess
import sys
import time

KEYWORDS = {
    "commit": "커밋해줘",
    "deploy": "배포해줘",
    "error": "에러 확인",
    "orchestration": "/orchestration",
}


class Typer:
    def __init__(self) -> None:
        self.pending_toast: str | None = None

    def on_event(self, event: dict) -> None:
        name = event.get("type")
        if not isinstance(name, str):
            return
        text = KEYWORDS.get(name)
        if text is None:
            print(f"[type] 모르는 키워드 버튼: {name}")
            self.pending_toast = "PC 도우미가 모르는 키워드입니다\n도우미를 새로 깔아 주세요"
            return
        error = _type(text)
        print(f"[type] \"{text}\" {'입력했습니다' if not error else '입력하지 못했습니다: ' + error}")
        self.pending_toast = f"입력했습니다: {text}" if not error else f"입력하지 못했습니다\n{error}"


def _type(text: str) -> str | None:
    """글을 입력한다. 잘 되면 None, 안 되면 이유."""
    try:
        if sys.platform == "win32":
            return _type_windows(text)
        if sys.platform == "darwin":
            return _type_mac(text)
        return "이 운영체제는 아직 안 됩니다"
    except Exception as e:  # 입력 실패로 도우미가 멈추지 않게 한다
        return str(e)[:60]


def _type_windows(text: str) -> str | None:
    import ctypes
    from ctypes import wintypes

    INPUT_KEYBOARD = 1
    KEYEVENTF_KEYUP = 0x0002
    KEYEVENTF_UNICODE = 0x0004

    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                    ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]

    class MOUSEINPUT(ctypes.Structure):  # INPUT 의 크기를 맞추려고만 둔다
        _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                    ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]

    class INPUT(ctypes.Structure):
        class _U(ctypes.Union):
            _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]

        _anonymous_ = ("u",)
        _fields_ = [("type", wintypes.DWORD), ("u", _U)]

    # UTF-16 단위로 누름·뗌을 보낸다 (한글은 글자마다 하나)
    units = text.encode("utf-16-le")
    codes = [int.from_bytes(units[i:i + 2], "little") for i in range(0, len(units), 2)]
    inputs = []
    for code in codes:
        for flags in (KEYEVENTF_UNICODE, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP):
            item = INPUT(type=INPUT_KEYBOARD)
            item.ki = KEYBDINPUT(wVk=0, wScan=code, dwFlags=flags, time=0, dwExtraInfo=0)
            inputs.append(item)
    array = (INPUT * len(inputs))(*inputs)
    sent = ctypes.windll.user32.SendInput(len(inputs), array, ctypes.sizeof(INPUT))
    if sent != len(inputs):
        return "Windows 가 입력을 막았습니다 (관리자 창이면 안 됩니다)"
    return None


def _type_mac(text: str) -> str | None:
    before = subprocess.run(["pbpaste"], capture_output=True).stdout
    subprocess.run(["pbcopy"], input=text.encode("utf-8"), check=True)
    result = subprocess.run(
        ["osascript", "-e", 'tell application "System Events" to keystroke "v" using command down'],
        capture_output=True, text=True, timeout=10)
    time.sleep(0.3)  # 붙여 넣기가 끝난 뒤 클립보드를 되돌린다
    subprocess.run(["pbcopy"], input=before)
    if result.returncode != 0:
        return "손쉬운 사용 권한이 필요합니다 (LLM Deck)"
    return None
