"""기기의 키워드 버튼({"type": 이름})을 누르면 이 PC 에서 지금 커서가 있는 곳에 정해 둔 글을 입력하고 Enter 를 누른다.

여기 적어 둔 글만 입력한다 (기기나 다른 사람이 아무 글·명령이나 치게 하지 못하게).
  - Windows: 글자를 하나씩 키보드 입력으로 보낸다 (SendInput 의 유니코드 입력. 한글도 한/영 상태와 상관없이 들어간다).
    관리자 권한으로 띄운 창에는 입력되지 않는다 (Windows 가 막는다).
  - Mac: 키보드 입력을 직접 보낸다 (CoreGraphics 의 유니코드 입력. 한/영 상태와 상관없고 클립보드를 건드리지 않는다).
    처음 한 번 시스템 설정 > 개인정보 보호 및 보안 > 손쉬운 사용 에서 "LLM Deck" 을 허용해야 한다 (처음 누를 때 허용 창이 뜬다).
결과는 기기 화면에 짧은 알림으로 보낸다 (pending_toast).
"""

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


def _type(text: str, enter: bool = True) -> str | None:
    """글을 입력한다 (enter 면 끝에 Enter). 잘 되면 None, 안 되면 이유."""
    try:
        if sys.platform == "win32":
            return _type_windows(text, enter)
        if sys.platform == "darwin":
            return _type_mac(text, enter)
        return "이 운영체제는 아직 안 됩니다"
    except Exception as e:  # 입력 실패로 도우미가 멈추지 않게 한다
        return str(e)[:60]


def _type_windows(text: str, enter: bool = True) -> str | None:
    import ctypes
    from ctypes import wintypes

    INPUT_KEYBOARD = 1
    KEYEVENTF_KEYUP = 0x0002
    KEYEVENTF_UNICODE = 0x0004
    VK_RETURN = 0x0D

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
    # 마지막에 Enter (보통 키로: 유니코드 입력의 줄바꿈은 프로그램마다 다르게 받는다)
    for flags in ((0, KEYEVENTF_KEYUP) if enter else ()):
        item = INPUT(type=INPUT_KEYBOARD)
        item.ki = KEYBDINPUT(wVk=VK_RETURN, wScan=0, dwFlags=flags, time=0, dwExtraInfo=0)
        inputs.append(item)
    array = (INPUT * len(inputs))(*inputs)
    sent = ctypes.windll.user32.SendInput(len(inputs), array, ctypes.sizeof(INPUT))
    if sent != len(inputs):
        return "Windows 가 입력을 막았습니다 (관리자 창이면 안 됩니다)"
    return None


_AX_PROMPTED = False  # 손쉬운 사용 허용 창은 도우미가 켜져 있는 동안 한 번만 띄운다


def _mac_trusted(cg, cf) -> bool:
    """손쉬운 사용에서 이 앱(LLM Deck)이 허용되어 있는지. 처음 한 번은 macOS 허용 창을 띄운다."""
    import ctypes

    global _AX_PROMPTED
    cg.AXIsProcessTrusted.restype = ctypes.c_bool
    if cg.AXIsProcessTrusted():
        return True
    if not _AX_PROMPTED:
        _AX_PROMPTED = True
        cf.CFDictionaryCreate.restype = ctypes.c_void_p
        cf.CFDictionaryCreate.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long,
                                          ctypes.c_void_p, ctypes.c_void_p)
        cf.CFRelease.argtypes = (ctypes.c_void_p,)
        cg.AXIsProcessTrustedWithOptions.restype = ctypes.c_bool
        cg.AXIsProcessTrustedWithOptions.argtypes = (ctypes.c_void_p,)
        keys = (ctypes.c_void_p * 1)(ctypes.c_void_p.in_dll(cg, "kAXTrustedCheckOptionPrompt").value)
        values = (ctypes.c_void_p * 1)(ctypes.c_void_p.in_dll(cf, "kCFBooleanTrue").value)
        options = cf.CFDictionaryCreate(None, keys, values, 1,
                                        ctypes.addressof(ctypes.c_void_p.in_dll(cf, "kCFTypeDictionaryKeyCallBacks")),
                                        ctypes.addressof(ctypes.c_void_p.in_dll(cf, "kCFTypeDictionaryValueCallBacks")))
        trusted = cg.AXIsProcessTrustedWithOptions(options)
        cf.CFRelease(options)
        return trusted
    return False


def _type_mac(text: str, enter: bool = True) -> str | None:
    """키보드 입력을 직접 만들어 보낸다 (CoreGraphics). 글자는 유니코드로 넣어 한/영 상태와 상관없고 클립보드도 쓰지 않는다.

    예전처럼 osascript(System Events)로 보내면 "자동화" 권한까지 따로 필요하고, 거절돼 있으면 10초 동안 멈췄다.
    이 방식은 손쉬운 사용 권한 하나만 있으면 된다.
    """
    import ctypes

    cg = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices")
    cf = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
    if not _mac_trusted(cg, cf):
        return "손쉬운 사용 권한이 필요합니다\n시스템 설정 > 개인정보 보호 및 보안\n> 손쉬운 사용에서 LLM Deck 켜기"

    cg.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
    cg.CGEventCreateKeyboardEvent.argtypes = (ctypes.c_void_p, ctypes.c_uint16, ctypes.c_bool)
    cg.CGEventKeyboardSetUnicodeString.argtypes = (ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_uint16))
    cg.CGEventSetFlags.argtypes = (ctypes.c_void_p, ctypes.c_uint64)
    cg.CGEventPost.argtypes = (ctypes.c_uint32, ctypes.c_void_p)
    cf.CFRelease.argtypes = (ctypes.c_void_p,)
    HID_TAP = 0  # kCGHIDEventTap: 실제 키보드에서 친 것처럼 지금 앞에 있는 창으로 간다
    RETURN = 36

    def post(keycode: int, units=None) -> None:
        for down in (True, False):
            event = cg.CGEventCreateKeyboardEvent(None, keycode, down)
            cg.CGEventSetFlags(event, 0)  # 사용자가 누르고 있는 Shift·Cmd 가 섞이지 않게
            if units:
                cg.CGEventKeyboardSetUnicodeString(event, len(units), (ctypes.c_uint16 * len(units))(*units))
            cg.CGEventPost(HID_TAP, event)
            cf.CFRelease(event)

    raw = text.encode("utf-16-le")
    codes = [int.from_bytes(raw[i:i + 2], "little") for i in range(0, len(raw), 2)]
    # 한 번에 너무 길면 앱이 잘라 받는다 (유니코드 글자는 이벤트 하나에 20개 정도까지)
    for i in range(0, len(codes), 16):
        post(0, codes[i:i + 16])
        time.sleep(0.01)
    if enter:
        time.sleep(0.05)
        post(RETURN)
    return None
