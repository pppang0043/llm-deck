"""기기의 키워드 버튼({"type": 이름})을 누르면 이 PC 에서 지금 커서가 있는 곳에 정해 둔 글을 입력하고 Enter 를 누른다.

여기 적어 둔 글(기본 목록이나 config.toml 의 [keywords])만 입력한다 (기기나 다른 사람이 아무 글·명령이나 치게 하지 못하게).
기기에는 버튼 이름과 id 만 보낸다 ({"keywords": ITEMS}). 기기는 누른 버튼의 id 만 돌려보내고, 글은 이 PC 에만 있다.
  - Windows: 글자를 하나씩 키보드 입력으로 보낸다 (SendInput 의 유니코드 입력. 한글도 한/영 상태와 상관없이 들어간다).
    관리자 권한으로 띄운 창에는 입력되지 않는다 (Windows 가 막는다).
  - Mac: 키보드 입력을 직접 보낸다 (CoreGraphics 의 유니코드 입력. 한/영 상태와 상관없고 클립보드를 건드리지 않는다).
    처음 한 번 시스템 설정 > 개인정보 보호 및 보안 > 손쉬운 사용 에서 "LLM Deck" 을 허용해야 한다 (처음 누를 때 허용 창이 뜬다).
결과는 기기 화면에 짧은 알림으로 보낸다 (pending_toast).
"""

import re
import subprocess
import sys
import time

# 기본 키워드 (config.toml 에 [keywords] 가 없을 때). 이름(id)은 예전 펌웨어도 그대로 보낸다.
# 글이 띄어쓰기로 끝나면 Enter 를 누르지 않는다 (/orchestration 은 뒤에 할 일을 이어 적는다)
DEFAULT_KEYWORDS = [
    ("commit", "커밋", "커밋해줘"),
    ("deploy", "배포", "배포해줘"),
    ("push", "푸쉬", "푸쉬해줘"),
    ("error", "에러 확인", "에러 확인"),
    ("orchestration", "/orch", "/orchestration "),
    ("clear", "/clear", "/clear"),
    ("compact", "/compact", "/compact"),
]
MAX_KEYWORDS = 7  # 기기 단축키 화면은 8칸 (마지막 칸은 Tab + Enter)
MAX_NAME = 12  # 버튼 이름 글자 수
MAX_TEXT = 200  # 입력할 글 글자 수

# 기기 버튼 이름(id) -> 입력할 글. 여기 있는 것만 입력한다. configure() 가 config.toml 대로 바꾼다.
KEYWORDS = {kid: text for kid, _, text in DEFAULT_KEYWORDS}
# 기기에 보내는 목록 [{"id", "name", "obj"}] (글은 PC 에만 둔다. obj 는 기기 알림 "PC에 ○○ 입력합니다" 의 ○○)
ITEMS: list[dict] = []


def _bad(value) -> bool:
    return not isinstance(value, str) or not value.strip() or any(ord(c) < 32 or ord(c) == 127 for c in value)


def particle(word: str) -> str:
    """word 뒤에 붙일 을/를. 마지막 글자가 받침으로 끝나면 을."""
    last = word.rstrip()[-1:].lower()
    if not last:
        return "를"
    code = ord(last)
    if 0xAC00 <= code <= 0xD7A3:
        return "을" if (code - 0xAC00) % 28 else "를"
    if last.isdigit():
        return "을" if last in "013678" else "를"  # 영 일 삼 육 칠 팔
    if last.isascii() and last.isalpha():
        return "을" if last in "bcdgklmnpt" else "를"  # 대충 소리로 (뒤의 e 는 읽지 않는 경우가 많아 를)
    return "을(를)"


def wants_enter(text: str) -> bool:
    """키워드 글 끝에 Enter 를 누를지. 띄어쓰기로 끝나면 이어 적도록 누르지 않는다."""
    return not text.endswith(" ")


def _item(kid: str, name: str, text: str) -> dict:
    # 알림에는 글을 보인다 (길면 버튼 이름)
    shown = text.strip() if len(text.strip()) <= 14 else name
    return {"id": kid, "name": name, "obj": f"\"{shown}\"{particle(shown)}"}


def configure(section: dict | None) -> None:
    """config.toml 의 [keywords] items = [{name, text}, ...] 를 읽는다. 없거나 쓸 것이 없으면 기본 목록."""
    global KEYWORDS, ITEMS
    raw = (section or {}).get("items")
    chosen = []
    if isinstance(raw, list):
        for n, entry in enumerate(raw, 1):
            name = entry.get("name") if isinstance(entry, dict) else None
            text = entry.get("text") if isinstance(entry, dict) else None
            if _bad(name) or _bad(text) or len(name.strip()) > MAX_NAME or len(text) > MAX_TEXT:
                print(f"[type] [keywords] {n}번째 키워드를 건너뜁니다 "
                      f"(name 은 {MAX_NAME}자, text 는 {MAX_TEXT}자까지, 줄바꿈·탭 없이)")
                continue
            if len(chosen) >= MAX_KEYWORDS:
                print(f"[type] [keywords] 키워드는 {MAX_KEYWORDS}개까지입니다. 나머지는 건너뜁니다.")
                break
            chosen.append((f"k{len(chosen)}", name.strip(), text))
        if not chosen:
            print("[type] [keywords] 쓸 수 있는 키워드가 없어 기본 목록을 씁니다.")
    entries = chosen or DEFAULT_KEYWORDS
    KEYWORDS = {kid: text for kid, _, text in entries}
    ITEMS = [_item(*e) for e in entries]


configure(None)
# 글 없이 키만 누르는 버튼 (이름 -> 화면 알림에 쓰는 이름, Enter 앞에 Tab 을 누를지).
# Tab + Enter 는 Claude Code 가 흐리게 보여준 추천 문장을 받아서 바로 보낸다.
# (ORCA 화면 아래 버튼은 앞에 있는 창이 아니라 고른 세션에 보내야 해서 orca.py 가 따로 맡는다)
KEY_PRESSES = {
    "tab_enter": ("Tab + Enter", True),
}


class Typer:
    def __init__(self) -> None:
        self.pending_toast: str | None = None

    def on_event(self, event: dict) -> None:
        name = event.get("type")
        if not isinstance(name, str):
            return
        if name in KEY_PRESSES:
            shown, tab = KEY_PRESSES[name]
            error = _type("", tab=tab)
            print(f"[type] {shown} {'눌렀습니다' if not error else '누르지 못했습니다: ' + error}")
            self.pending_toast = f"눌렀습니다: {shown}" if not error else f"누르지 못했습니다\n{error}"
            return
        text = KEYWORDS.get(name)
        if text is None:
            print(f"[type] 모르는 키워드 버튼: {name}")
            self.pending_toast = "PC 도우미가 모르는 키워드입니다\n도우미를 새로 깔아 주세요"
            return
        error = _type(text, enter=wants_enter(text))
        print(f"[type] \"{text}\" {'입력했습니다' if not error else '입력하지 못했습니다: ' + error}")
        self.pending_toast = f"입력했습니다: {text.strip()}" if not error else f"입력하지 못했습니다\n{error}"


def _type(text: str, enter: bool = True, tab: bool = False) -> str | None:
    """글을 입력한다 (tab 이면 글 뒤에 Tab, enter 면 끝에 Enter). 잘 되면 None, 안 되면 이유."""
    try:
        if sys.platform == "win32":
            return _type_windows(text, enter, tab)
        if sys.platform == "darwin":
            return _type_mac(text, enter, tab)
        return "이 운영체제는 아직 안 됩니다"
    except Exception as e:  # 입력 실패로 도우미가 멈추지 않게 한다
        return str(e)[:60]


MIRROR_TITLE = "LLM Deck 미러링"  # Windows 미러링 창 제목 (windows/app.pyw 의 TITLE)
# 돌아갈 창으로 고르지 않는 창 종류: 작업 표시줄, 바탕 화면
_SHELL_CLASSES = {"Shell_TrayWnd", "Shell_SecondaryTrayWnd", "Progman", "WorkerW"}


def _pick_previous(windows: list[dict]) -> int | None:
    """앞에서부터의 창 목록 (맨 앞이 지금 앞 창). 맨 앞이 미러링 창이면 그 뒤의 보통 창 (글을 쓰던 창), 아니면 None."""
    if not windows or windows[0]["title"] != MIRROR_TITLE:
        return None
    for w in windows[1:]:
        if (w["visible"] and not w["iconic"] and not w["owned"] and not w["tool"] and not w["cloaked"]
                and w["title"] and w["title"] != MIRROR_TITLE and w["cls"] not in _SHELL_CLASSES):
            return w["hwnd"]
    return None


def _windows_zorder(user32, limit: int = 1000) -> list[dict]:
    """Windows: 지금 앞 창부터 뒤로 차례로 (GetWindow 의 다음 창)."""
    import ctypes
    from ctypes import wintypes

    GW_HWNDNEXT, GW_OWNER, GWL_EXSTYLE, WS_EX_TOOLWINDOW, DWMWA_CLOAKED = 2, 4, -20, 0x80, 14
    user32.GetForegroundWindow.restype = wintypes.HWND
    user32.GetWindow.restype = wintypes.HWND
    user32.GetWindow.argtypes = (wintypes.HWND, wintypes.UINT)
    # 창 번호는 64비트 값이라 인자 형식을 적어 둔다 (적지 않으면 32비트 int 로 넘어간다)
    for name in ("IsWindowVisible", "IsIconic", "SetForegroundWindow"):
        getattr(user32, name).argtypes = (wintypes.HWND,)
    user32.GetWindowTextW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
    user32.GetClassNameW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
    user32.GetWindowLongW.argtypes = (wintypes.HWND, ctypes.c_int)
    try:
        dwm = ctypes.windll.dwmapi
    except OSError:
        dwm = None
    windows = []
    hwnd = user32.GetForegroundWindow()
    while hwnd and len(windows) < limit:
        title = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, title, 256)
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, cls, 256)
        cloaked = wintypes.DWORD(0)
        if dwm:  # 다른 가상 데스크톱의 창이나 숨은 스토어 앱 창은 보이는 것처럼 나와도 가려져 있다
            dwm.DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ctypes.byref(cloaked), ctypes.sizeof(cloaked))
        windows.append({"hwnd": hwnd, "title": title.value, "cls": cls.value,
                        "visible": bool(user32.IsWindowVisible(hwnd)), "iconic": bool(user32.IsIconic(hwnd)),
                        "owned": bool(user32.GetWindow(hwnd, GW_OWNER)),
                        "tool": bool(user32.GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW),
                        "cloaked": bool(cloaked.value)})
        hwnd = user32.GetWindow(hwnd, GW_HWNDNEXT)
    return windows


def _leave_mirror_windows() -> None:
    """Windows: 앞 창이 미러링 창이면 (미러링 창을 눌러 받아쓰기·키워드를 시작했다) 그 바로 뒤 창을 앞으로 되돌린다.
    그러지 않으면 글이 원래 쓰던 창이 아니라 미러링 창으로 간다 (Mac 은 _leave_own_window)."""
    import ctypes

    try:
        user32 = ctypes.windll.user32
        target = _pick_previous(_windows_zorder(user32))
        if not target:
            return
        # 뒤에서 도는 프로그램은 창을 앞으로 못 띄우게 막혀 있다. Alt 키를 한 번 눌렀다 떼면 허용된다 (orca._front_windows)
        user32.keybd_event(0x12, 0, 0, 0)
        user32.keybd_event(0x12, 0, 2, 0)
        user32.SetForegroundWindow(target)
        # 그 창이 앞으로 와서 커서를 받을 때까지 (1.5초까지) 기다린다
        until = time.monotonic() + 1.5
        while time.monotonic() < until:
            if user32.GetForegroundWindow() == target:
                time.sleep(0.15)
                break
            time.sleep(0.05)
        print("[type] 미러링 창이 앞에 있어 그 뒤 창으로 돌아가 입력합니다")
    except Exception as e:  # 못 돌아가도 입력은 한다
        print(f"[type] 앞에 있던 창으로 돌아가지 못했습니다: {e}")


def _type_windows(text: str, enter: bool = True, tab: bool = False) -> str | None:
    import ctypes
    from ctypes import wintypes

    INPUT_KEYBOARD = 1
    KEYEVENTF_KEYUP = 0x0002
    KEYEVENTF_UNICODE = 0x0004
    VK_RETURN = 0x0D
    VK_TAB = 0x09

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
    def key(vk: int) -> list:
        items = []
        for flags in (0, KEYEVENTF_KEYUP):
            item = INPUT(type=INPUT_KEYBOARD)
            item.ki = KEYBDINPUT(wVk=vk, wScan=0, dwFlags=flags, time=0, dwExtraInfo=0)
            items.append(item)
        return items

    def send(items: list) -> bool:
        array = (INPUT * len(items))(*items)
        return ctypes.windll.user32.SendInput(len(items), array, ctypes.sizeof(INPUT)) == len(items)

    # 그다음 Tab, 마지막에 Enter (보통 키로: 유니코드 입력의 줄바꿈은 프로그램마다 다르게 받는다).
    # Tab 과 Enter 를 한꺼번에 보내면 붙여넣기로 받는 프로그램이 있어 사이를 조금 띄운다.
    batches = [inputs + key(VK_TAB)] if tab else [inputs]
    _leave_mirror_windows()
    if enter:
        batches.append(key(VK_RETURN))
    for i, items in enumerate(b for b in batches if b):
        if i:
            time.sleep(0.1)
        if not send(items):
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


OWN_BUNDLE = "com.llmdeck.companion"  # 이 도우미의 Mac 앱 (미러링 창)


def _bundle(asn: str) -> str:
    out = subprocess.run(["lsappinfo", "info", "-only", "bundleid", asn], capture_output=True, text=True, timeout=3).stdout
    match = re.search(r'="(.*)"', out)
    return match.group(1) if match else ""


def _leave_own_window() -> None:
    """앞 앱이 LLM Deck 이면 (미러링 창을 눌러 받아쓰기·키워드를 시작했다) 바로 뒤에 있던 앱을 앞으로 되돌린다.
    그러지 않으면 글이 원래 쓰던 창이 아니라 미러링 창으로 간다. 앞 앱 순서는 lsappinfo 가 앞에서부터 알려 준다."""
    try:
        out = subprocess.run(["lsappinfo", "visibleProcessList"], capture_output=True, text=True, timeout=3).stdout
        apps = re.findall(r"ASN:0x[0-9a-f]+-0x[0-9a-f]+", out)
        if len(apps) < 2 or _bundle(apps[0]) != OWN_BUNDLE:
            return
        target = next((b for b in map(_bundle, apps[1:]) if b and b != OWN_BUNDLE), "")
        if not target:
            return
        subprocess.run(["open", "-b", target], capture_output=True, timeout=5)
        # 그 앱이 앞으로 와서 커서를 받을 때까지 (1.5초까지) 기다린다
        until = time.monotonic() + 1.5
        while time.monotonic() < until:
            front = subprocess.run(["lsappinfo", "front"], capture_output=True, text=True, timeout=3).stdout.strip()
            if front and _bundle(front.rstrip(":")) == target:
                time.sleep(0.15)
                break
            time.sleep(0.05)
        print(f"[type] 미러링 창이 앞에 있어 {target} 로 돌아가 입력합니다")
    except (OSError, subprocess.SubprocessError) as e:
        print(f"[type] 앞에 있던 앱으로 돌아가지 못했습니다: {e}")


def _type_mac(text: str, enter: bool = True, tab: bool = False) -> str | None:
    """키보드 입력을 직접 만들어 보낸다 (CoreGraphics). 글자는 유니코드로 넣어 한/영 상태와 상관없고 클립보드도 쓰지 않는다.

    예전처럼 osascript(System Events)로 보내면 "자동화" 권한까지 따로 필요하고, 거절돼 있으면 10초 동안 멈췄다.
    이 방식은 손쉬운 사용 권한 하나만 있으면 된다.
    """
    import ctypes

    cg = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices")
    cf = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
    if not _mac_trusted(cg, cf):
        return "손쉬운 사용 권한이 필요합니다\n시스템 설정 > 개인정보 보호 및 보안\n> 손쉬운 사용에서 LLM Deck 켜기"
    _leave_own_window()

    cg.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
    cg.CGEventCreateKeyboardEvent.argtypes = (ctypes.c_void_p, ctypes.c_uint16, ctypes.c_bool)
    cg.CGEventKeyboardSetUnicodeString.argtypes = (ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_uint16))
    cg.CGEventSetFlags.argtypes = (ctypes.c_void_p, ctypes.c_uint64)
    cg.CGEventPost.argtypes = (ctypes.c_uint32, ctypes.c_void_p)
    cf.CFRelease.argtypes = (ctypes.c_void_p,)
    HID_TAP = 0  # kCGHIDEventTap: 실제 키보드에서 친 것처럼 지금 앞에 있는 창으로 간다
    RETURN = 36
    TAB = 48

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
    if tab:
        post(TAB)
        time.sleep(0.1)  # 추천 문장이 입력칸에 들어갈 틈
    if enter:
        time.sleep(0.05)
        post(RETURN)
    return None
