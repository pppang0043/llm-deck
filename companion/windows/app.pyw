"""LLM Deck 도우미 앱 (Windows). 작업 표시줄 오른쪽 아래(트레이)에 아이콘을 두고, 파이썬 도우미(main.py)를 자식 프로세스로 띄워 둔다.
트레이 아이콘에서 미러링 창을 연다: 작은 창에 기기 화면을 띄우고, 눌러서 기기를 조작한다 (USB 나 Wi-Fi 로 연결했을 때).
미러링 창은 도우미(mirror.py)가 이 PC 안에 띄운 http://127.0.0.1:4013/ 에서 화면을 받아 가고 누르기를 보낸다.
하는 일은 mirror.py 의 PAGE 와 같다. 이 창이 화면을 받아 가는 동안만 기기가 화면을 보낸다.

    pythonw app.pyw               (시작 메뉴) 도우미가 꺼져 있으면 켜고, 미러링 창을 연다
    pythonw app.pyw --background  (로그인할 때) 도우미만 켠다
    pythonw app.pyw --mirror      (mirror.cmd, main.py --mirror) 미러링 창만. 앱이 떠 있으면 그 앱이 연다
    python app.pyw --write-icon F 시작 메뉴 바로 가기에 쓸 아이콘(.ico)을 만든다 (install.ps1)

예전에는 C# 으로 만든 LLM Deck.exe 였다. 설치할 때 새로 만든 exe 를 회사 보안 프로그램(Symantec 등)이
처음 보는 프로그램이라고 막아서, Python 재단이 서명한 pythonw.exe 로 돌리는 이 파일로 바꿨다.
창은 Python 에 들어 있는 tkinter, 트레이 아이콘은 pystray 로 그린다.
"""

import argparse
import base64
import json
import os
import queue
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

COMPANION = Path(__file__).resolve().parents[1]
BASE = "http://127.0.0.1:4013/"
TITLE = "LLM Deck 미러링"
DISCONNECTED = "도우미와 연결이 끊겼습니다. 도우미가 켜져 있는지 확인하세요"
WAITING = "기기 화면을 기다리는 중..."
LOG = Path.home() / ".llm-deck" / "companion.log"
BACK = "#0b0e11"
# 앱이 이미 떠 있을 때 한 번 더 열면 이 신호로 떠 있는 앱에 창을 열라고 알린다 (mirror.py 도 같은 이름을 쓴다)
OPEN_SIGNAL = "LLMDeckOpenMirror"
# 앱 없이 미러링 창만 띄울 때 하나만 뜨게
MIRROR_ONLY = "LLMDeckMirror"
# 설정(항상 위, 배율, 창 위치)을 기억하는 곳. 예전 LLM Deck.exe 와 같은 자리라 고른 것이 그대로 이어진다
REG_PATH = r"Software\LLMDeck\Mirror"
# 이 PC 안으로 바로 간다 (회사 프록시 설정이 있어도). 브라우저가 아니라서 Mozilla 를 붙이지 않는다 (mirror.py 가 앱 창으로 센다)
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

if sys.platform == "win32":
    import ctypes
    import winreg
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32.CreateEventW.restype = wintypes.HANDLE
    kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.OpenEventW.restype = wintypes.HANDLE
    kernel32.OpenEventW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.SetEvent.argtypes = [wintypes.HANDLE]
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    user32.FindWindowW.restype = wintypes.HWND
    user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.SetForegroundWindow.argtypes = [wintypes.HWND]
    user32.IsIconic.argtypes = [wintypes.HWND]
    user32.MonitorFromPoint.restype = wintypes.HANDLE
    user32.MonitorFromPoint.argtypes = [wintypes.POINT, wintypes.DWORD]

    class MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT), ("rcWork", wintypes.RECT),
                    ("dwFlags", wintypes.DWORD)]

    user32.GetMonitorInfoW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MONITORINFO)]
    user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]

    ERROR_ALREADY_EXISTS = 183
    EVENT_MODIFY_STATE = 0x0002
    INFINITE = 0xFFFFFFFF

    # JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    class _IoCounters(ctypes.Structure):
        _fields_ = [(n, ctypes.c_ulonglong) for n in ("r", "w", "o", "rb", "wb", "ob")]

    class _BasicLimits(ctypes.Structure):
        _fields_ = [
            ("PerProcessUserTimeLimit", ctypes.c_int64),
            ("PerJobUserTimeLimit", ctypes.c_int64),
            ("LimitFlags", wintypes.DWORD),
            ("MinimumWorkingSetSize", ctypes.c_size_t),
            ("MaximumWorkingSetSize", ctypes.c_size_t),
            ("ActiveProcessLimit", wintypes.DWORD),
            ("Affinity", ctypes.c_size_t),
            ("PriorityClass", wintypes.DWORD),
            ("SchedulingClass", wintypes.DWORD),
        ]

    class _JobLimits(ctypes.Structure):
        _fields_ = [
            ("BasicLimitInformation", _BasicLimits),
            ("IoInfo", _IoCounters),
            ("ProcessMemoryLimit", ctypes.c_size_t),
            ("JobMemoryLimit", ctypes.c_size_t),
            ("PeakProcessMemoryUsed", ctypes.c_size_t),
            ("PeakJobMemoryUsed", ctypes.c_size_t),
        ]


# ---- 설정 ----

def read_setting(name: str, fallback: int) -> int:
    try:
        if sys.platform == "win32":
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH) as key:
                value, kind = winreg.QueryValueEx(key, name)
                return value if kind == winreg.REG_DWORD else fallback
        # Windows 가 아닐 때(개발용)는 파일에
        return int(json.loads((LOG.parent / "mirror-window.json").read_text()).get(name, fallback))
    except (OSError, ValueError, TypeError):
        return fallback


def save_setting(name: str, value: int) -> None:
    try:
        if sys.platform == "win32":
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, REG_PATH) as key:
                # 창 위치는 음수일 수 있다 (왼쪽 모니터). DWORD 로 넣을 수 있게 바꾼다
                winreg.SetValueEx(key, name, 0, winreg.REG_DWORD, value & 0xFFFFFFFF)
            return
        path = LOG.parent / "mirror-window.json"
        data = json.loads(path.read_text()) if path.exists() else {}
        data[name] = value
        path.write_text(json.dumps(data))
    except (OSError, ValueError):
        pass


def signed(v: int) -> int:
    return v - (1 << 32) if v >= 1 << 31 else v


def read_position(name: str) -> int | None:
    """기억해 둔 창 위치 값 (음수일 수 있다). 없으면 None"""
    v = signed(read_setting(name, -1 << 31) & 0xFFFFFFFF)
    return None if v == -1 << 31 else v


def mirror_origin(size: tuple[int, int], corner: tuple[int, int], area: tuple[int, int, int, int]) -> tuple[int, int]:
    """창 오른쪽 아래 모서리를 corner 에 둘 때의 왼쪽 위. area (왼, 위, 오른, 아래) 밖으로는 안 나가게"""
    w, h = size
    x = min(max(corner[0] - w, area[0]), area[2] - w)
    y = min(max(corner[1] - h, area[1]), area[3] - h)
    return x, y


def work_area(x: int, y: int, primary: bool = False) -> tuple[int, int, int, int] | None:
    """Windows: (x, y) 가 있는 모니터에서 작업 표시줄을 뺀 영역. 그 자리에 모니터가 없으면 None. primary 면 메인 모니터"""
    hmon = user32.MonitorFromPoint(wintypes.POINT(x, y), 1 if primary else 0)  # 1 = 메인 모니터, 0 = 없으면 NULL
    info = MONITORINFO(cbSize=ctypes.sizeof(MONITORINFO))
    if not hmon or not user32.GetMonitorInfoW(hmon, ctypes.byref(info)):
        return None
    r = info.rcWork
    return r.left, r.top, r.right, r.bottom


# ---- 기기 화면 풀기 ----

# 기기가 줄인 화면을 푼다 (펌웨어 mirror.cpp 의 pack): 머리 h < 128 이면 h+1 점 그대로, 아니면 한 점을 h-126 번.
# 한 점은 2바이트 작은 끝 RGB565. 브라우저 페이지처럼 5비트·6비트 색을 0..255 로 늘린다 (.5 는 올림)
R5 = bytes(int(i * 255 / 31 + 0.5) for i in range(32))
G6 = bytes(int(i * 255 / 63 + 0.5) for i in range(64))


def unpack(src: bytes, n: int) -> bytes:
    """줄인 화면을 RGB 바이트(점마다 3바이트)로 푼다"""
    import numpy as np

    px = np.zeros(n, dtype=np.uint16)
    o = p = 0
    size = len(src)
    while o < size and p < n:
        head = src[o]
        o += 1
        if head < 128:
            count = min(head + 1, n - p, (size - o) // 2)
            px[p : p + count] = np.frombuffer(src, dtype="<u2", count=count, offset=o)
            o += (head + 1) * 2
            p += count
        else:
            if o + 1 >= size:
                break
            count = min(head - 126, n - p)
            px[p : p + count] = src[o] | (src[o + 1] << 8)
            o += 2
            p += count
    rgb = np.empty((n, 3), dtype=np.uint8)
    rgb[:, 0] = np.frombuffer(R5, dtype=np.uint8)[px >> 11]
    rgb[:, 1] = np.frombuffer(G6, dtype=np.uint8)[(px >> 5) & 63]
    rgb[:, 2] = np.frombuffer(R5, dtype=np.uint8)[px & 31]
    return rgb.tobytes()


# ---- 미러링 창 ----

class MirrorWindow:
    """기기 화면을 띄우고 누르기를 보내는 창. 닫으면 화면 받기를 멈춘다 (열 때마다 새로 만든다)"""

    def __init__(self, root, on_closed=None):
        import tkinter as tk

        self.tk = tk
        self.root = root
        self.on_closed = on_closed
        self.closing = False
        self.w = self.h = 0
        self.zoom = max(1, min(3, read_setting("Zoom", 1)))
        self.top_var = tk.BooleanVar(value=read_setting("TopMost", 1) != 0)  # 처음에는 항상 위
        self.zoom_var = tk.IntVar(value=self.zoom)
        self.state = WAITING
        self.b64 = False
        self.frames: queue.Queue = queue.Queue(maxsize=2)  # 받는 스레드 → 창 스레드
        self.outbox: queue.Queue = queue.Queue()  # 누르기를 보낸 차례대로 기기에 닿게 한 줄로 보낸다
        self.pressing = False
        self.last_move = self.last_swipe = 0.0
        self.wheel = 0
        self.held: set[str] = set()
        self.placed = False
        self.save_job = None

        win = self.win = tk.Toplevel(root)
        win.title(TITLE)
        win.configure(background=BACK)
        win.resizable(False, False)
        win.protocol("WM_DELETE_WINDOW", self.close)
        self.canvas = tk.Canvas(win, background=BACK, highlightthickness=0, borderwidth=0, cursor="hand2")
        self.canvas.pack()
        self.src = tk.PhotoImage()  # 받은 화면 (기기 크기)
        self.shown = tk.PhotoImage()  # 배율만큼 키운 것
        self.image = self.canvas.create_image(0, 0, anchor="nw", image=self.shown, state="hidden")
        self.shade = self.canvas.create_rectangle(0, 0, 0, 0, fill=BACK, outline="", state="hidden")
        self.text = self.canvas.create_text(0, 0, fill="#c8d0d8", font=("Malgun Gothic", 10), justify="center")
        self.fit()
        self.place()
        self.apply_top()

        menu = self.menu = tk.Menu(win, tearoff=0)
        menu.add_checkbutton(label="항상 위", variable=self.top_var, command=self.toggle_top)
        menu.add_separator()
        for z in (1, 2, 3):
            menu.add_radiobutton(label=f"{z}배", variable=self.zoom_var, value=z, command=lambda z=z: self.set_zoom(z))
        menu.add_separator()
        menu.add_command(label="홈", accelerator="Esc", command=self.home)
        menu.add_command(label="닫기", command=self.close)

        c = self.canvas
        c.bind("<ButtonPress-1>", self.mouse_down)
        c.bind("<B1-Motion>", self.mouse_move)
        c.bind("<ButtonRelease-1>", self.mouse_up)
        c.bind("<Button-3>", lambda e: menu.tk_popup(e.x_root, e.y_root))
        win.bind("<MouseWheel>", self.mouse_wheel)
        for key, (dx, dy) in {"Down": (0, -1), "Up": (0, 1), "Right": (-1, 0), "Left": (1, 0)}.items():
            win.bind(f"<KeyPress-{key}>", lambda e, k=key, d=(dx, dy): self.arrow(k, *d))
            win.bind(f"<KeyRelease-{key}>", lambda e, k=key: self.held.discard(k))
        win.bind("<Escape>", lambda e: self.home())
        win.bind("<Configure>", self.moved)

        threading.Thread(target=self.poll, daemon=True).start()
        threading.Thread(target=self.send, daemon=True).start()
        self.draw_state()
        self.pump()

    # 창 안쪽 크기 = 기기 화면 크기 × 배율 (첫 장이 오기 전에는 320x240). 창 오른쪽 아래 모서리는 그대로
    def fit(self) -> None:
        before = self.outer() if self.placed else None
        width, height = (self.w or 320) * self.zoom, (self.h or 240) * self.zoom
        self.canvas.configure(width=width, height=height)
        self.canvas.coords(self.shade, 0, 0, width, height)
        self.canvas.coords(self.text, width / 2, height / 2)
        self.canvas.itemconfigure(self.text, width=max(width - 24, 60))
        if before:
            corner = before[2], before[3]
            self.move_corner(corner, self.area_at(corner[0] - 20, corner[1] - 20) or self.main_area())

    # 창 바깥 테두리 (왼, 위, 오른, 아래)
    def outer(self) -> tuple[int, int, int, int]:
        self.win.update_idletasks()
        if sys.platform == "win32":
            r = wintypes.RECT()
            user32.GetWindowRect(int(self.win.wm_frame(), 16), ctypes.byref(r))
            return r.left, r.top, r.right, r.bottom
        x, y = self.win.winfo_x(), self.win.winfo_y()
        return x, y, x + self.win.winfo_width(), y + self.win.winfo_height()

    def area_at(self, x: int, y: int) -> tuple[int, int, int, int] | None:
        if sys.platform == "win32":
            return work_area(x, y)
        sw, sh = self.win.winfo_screenwidth(), self.win.winfo_screenheight()
        return (0, 0, sw, sh) if 0 <= x < sw and 0 <= y < sh else None

    # 메인 모니터 (작업 표시줄을 뺀 영역). 모니터를 붙였다 떼도 이것을 기준으로 위치를 잡는다
    def main_area(self) -> tuple[int, int, int, int]:
        if sys.platform == "win32":
            area = work_area(0, 0, primary=True)
            if area:
                return area
        return 0, 0, self.win.winfo_screenwidth(), self.win.winfo_screenheight()

    def move_corner(self, corner: tuple[int, int], area: tuple[int, int, int, int]) -> None:
        left, top, right, bottom = self.outer()
        x, y = mirror_origin((right - left, bottom - top), corner, area)
        self.win.geometry(f"+{x}+{y}")

    # 지난번 닫은 위치에 띄운다 (창 오른쪽 아래 모서리 기준). 그 모니터가 지금 없으면 메인 모니터 오른쪽 아래에서
    # 지난번만큼 떨어진 곳에 띄운다 (처음에는 12점). 화면 밖으로는 안 나가게
    def place(self) -> None:
        right, bottom = read_position("Right"), read_position("Bottom")
        area = self.area_at(right - 20, bottom - 20) if right is not None and bottom is not None else None
        if area:
            corner = right, bottom
        else:
            area = self.main_area()
            corner = area[2] - read_setting("MainRight", 12), area[3] - read_setting("MainBottom", 12)
        self.move_corner(corner, area)
        self.placed = True

    # 창을 옮기면 (잠깐 멈춘 뒤) 위치를 기억한다. 앱이 갑자기 꺼져도 마지막 위치가 남는다
    def moved(self, e) -> None:
        if e.widget is not self.win or not self.placed or self.closing:
            return
        if self.save_job:
            self.win.after_cancel(self.save_job)
        self.save_job = self.win.after(500, self.remember)

    # 창 오른쪽 아래 모서리를 기억한다. 메인 모니터에 있으면 메인 모니터 기준 위치도 기억한다
    def remember(self) -> None:
        self.save_job = None
        if self.win.state() != "normal":
            return
        left, top, right, bottom = self.outer()
        save_setting("Right", right)
        save_setting("Bottom", bottom)
        main = self.main_area()
        if main[0] <= (left + right) // 2 < main[2] and main[1] <= (top + bottom) // 2 < main[3]:
            save_setting("MainRight", max(0, main[2] - right))
            save_setting("MainBottom", max(0, main[3] - bottom))

    def apply_top(self) -> None:
        self.win.attributes("-topmost", self.top_var.get())

    def toggle_top(self) -> None:
        save_setting("TopMost", 1 if self.top_var.get() else 0)
        self.apply_top()

    def set_top(self, on: bool) -> None:
        self.top_var.set(on)
        self.toggle_top()

    def set_zoom(self, z: int) -> None:
        self.zoom = z
        self.zoom_var.set(z)
        save_setting("Zoom", z)
        self.fit()
        self.redraw()

    def show(self) -> None:
        win = self.win
        win.deiconify()
        win.lift()
        win.focus_force()
        # 앞으로 가져오기만 하고 항상 위 설정은 그대로 둔다
        if not self.top_var.get():
            win.attributes("-topmost", True)
            win.after(200, self.apply_top)

    def close(self) -> None:
        if self.closing:
            return
        self.closing = True
        self.outbox.put(None)
        if self.save_job:
            self.win.after_cancel(self.save_job)
        self.remember()
        self.win.destroy()
        if self.on_closed:
            self.on_closed(self)

    # ---- 화면 ----

    def set_state(self, text: str | None) -> None:
        if self.state != text:
            self.state = text
            self.draw_state()

    def draw_state(self) -> None:
        c = self.canvas
        if self.state is None:
            c.itemconfigure(self.text, state="hidden")
            c.itemconfigure(self.shade, state="hidden")
            return
        c.itemconfigure(self.text, text=self.state, state="normal")
        # 화면이 있으면 그 위를 어둡게 덮고 안내를 띄운다
        c.itemconfigure(self.shade, state="normal" if self.w else "hidden")
        c.tag_raise(self.shade)
        c.tag_raise(self.text)

    def redraw(self) -> None:
        if not self.w:
            return
        self.shown.configure(width=self.w * self.zoom, height=self.h * self.zoom)
        # 확대해도 흐려지지 않게 점을 그대로 키운다
        self.shown.tk.call(self.shown, "copy", self.src, "-zoom", self.zoom, self.zoom)
        self.canvas.itemconfigure(self.image, state="normal")

    def show_frame(self, w: int, h: int, rgb: bytes) -> None:
        resized = (w, h) != (self.w, self.h)
        self.w, self.h = w, h  # 처음이거나 기기 화면을 돌렸다 (320x240 ↔ 240x320)
        ppm = b"P6 %d %d 255\n" % (w, h) + rgb
        # Tk 9 는 그대로만, 예전 Tk 8.6 은 base64 로만 받는 것이 있어 되는 쪽을 기억해 둔다
        if self.b64:
            self.src.configure(data=base64.b64encode(ppm), format="PPM")
        else:
            try:
                self.src.configure(data=ppm, format="PPM")
            except self.tk.TclError:
                self.b64 = True
                self.src.configure(data=base64.b64encode(ppm), format="PPM")
        if resized:
            self.fit()
        self.redraw()
        self.set_state(None)

    # 받는 스레드가 넣어 둔 것을 창 스레드에서 그린다
    def pump(self) -> None:
        if self.closing:
            return
        try:
            while True:
                kind, value = self.frames.get_nowait()
                if kind == "frame":
                    self.show_frame(*value)
                else:
                    self.set_state(value)
        except queue.Empty:
            pass
        self.win.after(15, self.pump)

    def put(self, kind: str, value) -> None:
        if kind == "frame":
            try:
                self.frames.put_nowait((kind, value))
            except queue.Full:
                pass  # 창이 바빠 밀렸으면 이번 장은 건너뛴다
        else:
            self.frames.put((kind, value))

    # 새 장이 올 때까지 기다렸다 받기를 되풀이한다 (도우미가 1.5초까지 붙잡고 있다가 204 로 "새 장 없음")
    def poll(self) -> None:
        seq = 0
        while not self.closing:
            try:
                with OPENER.open(f"{BASE}frame?after={seq}", timeout=15) as res:
                    if res.status != 200:
                        continue
                    nxt = int(res.headers["X-Seq"])
                    w, h = int(res.headers["X-Width"]), int(res.headers["X-Height"])
                    body = res.read()
                if w <= 0 or h <= 0 or w * h > 4096 * 4096:
                    raise ValueError("이상한 화면 크기")
                seq = nxt
                self.put("frame", (w, h, unpack(body, w * h)))
            except urllib.error.HTTPError as e:
                if e.code == 503:
                    # 기기가 USB·Wi-Fi 로 연결되어 있지 않다. 도우미가 보낸 안내를 그대로 보여 준다 (도우미가 1초 쉬고 대답한다)
                    self.put("state", e.read().decode("utf-8", "replace"))
                else:
                    self.put("state", DISCONNECTED)
                    time.sleep(2)
            except Exception:
                # 도우미가 꺼졌거나 받은 장이 이상하다. 브라우저 페이지처럼 잠시 쉬고 다시 받는다
                self.put("state", DISCONNECTED)
                time.sleep(2)

    # ---- 누르기 ----

    # 누르기를 한 줄로 보낸다. 도우미가 꺼져 있으면 버린다 (화면에 안내가 뜬다)
    def send(self) -> None:
        while True:
            job = self.outbox.get()
            if job is None:
                return
            path, body = job
            req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(), method="POST",
                                         headers={"Content-Type": "application/json"})  # 도우미가 이 형식만 받는다
            try:
                OPENER.open(req, timeout=5).close()
            except OSError:
                pass

    def post(self, path: str, body: dict) -> None:
        if not self.closing:
            self.outbox.put((path, body))

    def home(self) -> None:
        self.post("home", {})

    # 창 위치를 기기 픽셀로 바꿔 보낸다. 창 밖으로 끌면 가장자리로 자른다
    def touch(self, cx: int, cy: int, down: bool) -> None:
        if not self.w:
            return
        x = max(0, min(self.w - 1, cx // self.zoom))
        y = max(0, min(self.h - 1, cy // self.zoom))
        self.post("touch", {"x": x, "y": y, "down": down})

    def mouse_down(self, e) -> None:
        self.pressing = True
        self.touch(e.x, e.y, True)

    def mouse_move(self, e) -> None:
        if not self.pressing or time.monotonic() - self.last_move < 0.03:
            return
        self.last_move = time.monotonic()
        self.touch(e.x, e.y, True)

    def mouse_up(self, e) -> None:
        if not self.pressing:
            return
        self.pressing = False
        self.touch(e.x, e.y, False)

    # 밀기: 화면 가운데를 지나 한쪽에서 다른 쪽으로 끄는 점들 (기기가 10ms 마다 하나씩 쓴다)
    def swipe(self, dx: int, dy: int) -> None:
        if not self.w:
            return
        w, h = self.w, self.h
        x0, y0 = w / 2 - dx * w * 0.35, h / 2 - dy * h * 0.3

        def rnd(v: float) -> int:
            return int(v + 0.5) if v >= 0 else -int(-v + 0.5)  # 자바스크립트 Math.round 처럼

        points = [{"x": rnd(x0 + dx * w * 0.7 * i / 8), "y": rnd(y0 + dy * h * 0.6 * i / 8), "down": True} for i in range(9)]
        points.append({**points[-1], "down": False})
        self.post("touch", {"points": points})

    def mouse_wheel(self, e) -> None:
        self.wheel -= e.delta  # 브라우저처럼 아래로 굴리면 +
        now = time.monotonic()
        if abs(self.wheel) < 40 or now - self.last_swipe < 0.35:
            return
        self.swipe(0, -1 if self.wheel > 0 else 1)  # 아래로 굴리면 다음 쪽 (손가락을 위로 미는 것)
        self.wheel = 0
        self.last_swipe = now

    # 방향키로 밀기. 키를 누르고 있어도 한 번만 민다
    def arrow(self, key: str, dx: int, dy: int) -> None:
        if key in self.held:
            return
        self.held.add(key)
        self.swipe(dx, dy)


# ---- 트레이 앱 ----

def tray_image(size: int = 64):
    """트레이 아이콘: 주황 바탕에 작은 화면"""
    from PIL import Image, ImageDraw

    img = Image.new("RGBA", (size, size), (0xD9, 0x77, 0x57, 255))
    m = size // 5
    ImageDraw.Draw(img).rectangle([m, m + size // 10, size - m - 1, size - m - size // 10 - 1], fill=(0x10, 0x14, 0x18, 255))
    return img


class Job:
    """도우미를 묶어 둔다. 앱이 어떻게 끝나든 Windows 가 묶음째 끈다"""

    def __init__(self) -> None:
        self.handle = kernel32.CreateJobObjectW(None, None)
        limits = _JobLimits()
        limits.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        kernel32.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits))  # 9 = 확장 제한

    def add(self, proc: subprocess.Popen) -> None:
        kernel32.AssignProcessToJobObject(self.handle, int(proc._handle))

    def kill(self) -> None:
        if self.handle:
            kernel32.TerminateJobObject(self.handle, 1)
            kernel32.CloseHandle(self.handle)
            self.handle = None


class DeckApp:
    """트레이 아이콘과 도우미 자식 프로세스. 미러링 창은 열 때마다 새로 만든다 (닫으면 화면 받기를 멈춘다)"""

    def __init__(self, root, open_event) -> None:
        import pystray

        self.root = root
        self.open_event = open_event
        self.mirror: MirrorWindow | None = None
        self.helper: subprocess.Popen | None = None
        self.job: Job | None = None
        self.restarting = self.quitting = False
        self.jobs: queue.Queue = queue.Queue()  # 트레이·신호 스레드에서 온 일을 창 스레드로 넘긴다

        def later(fn):
            return lambda icon, item: self.jobs.put(fn)

        item = pystray.MenuItem
        self.icon = pystray.Icon("LLM Deck", tray_image(), "LLM Deck", menu=pystray.Menu(
            item("미러링 열기", later(self.open_mirror), default=True),  # 아이콘을 누르면 하는 일
            # 미러링 창의 오른쪽 클릭 메뉴와 같은 설정 (창이 닫혀 있으면 다음에 열 때 쓴다)
            item("항상 위", later(self.toggle_top), checked=lambda item: read_setting("TopMost", 1) != 0),
            pystray.Menu.SEPARATOR,
            item("도우미 다시 시작", later(self.restart)),
            item("기록 보기", later(self.show_log)),
            pystray.Menu.SEPARATOR,
            item("종료 (다음 로그인 때 다시 켜짐)", later(self.quit)),
        ))
        self.icon.run_detached()
        self.start_helper()
        threading.Thread(target=self.listen, daemon=True).start()
        self.pump()

    def pump(self) -> None:
        try:
            while True:
                self.jobs.get_nowait()()
        except queue.Empty:
            pass
        if not self.quitting:
            self.root.after(50, self.pump)

    # 한 번 더 연 앱이나 도우미(mirror.py)가 신호를 주면 창을 연다
    def listen(self) -> None:
        while kernel32.WaitForSingleObject(self.open_event, INFINITE) == 0:
            self.jobs.put(self.open_mirror)

    def start_helper(self) -> None:
        # python.exe 를 창 없이 띄운다 (pythonw 로 띄우면 도우미가 부르는 프로그램마다 검은 창이 뜬다)
        python = Path(sys.executable).with_name("python.exe")
        LOG.parent.mkdir(parents=True, exist_ok=True)
        self.job = Job()
        try:
            proc = subprocess.Popen([str(python), "main.py", "--log", str(LOG)], cwd=COMPANION,
                                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
        except OSError as e:
            self.message(f"도우미를 켜지 못했습니다: {e}")
            self.quit()
            return
        self.job.add(proc)
        self.helper = proc
        threading.Thread(target=lambda: (proc.wait(), self.jobs.put(lambda: self.helper_exited(proc))), daemon=True).start()

    def helper_exited(self, which: subprocess.Popen) -> None:
        if which is not self.helper or self.quitting:
            return
        if self.restarting:
            self.restarting = False
            self.start_helper()
            return
        # 도우미가 스스로 끝났다 (이미 다른 도우미가 돌고 있거나 설정이 틀렸다). 이유는 기록 파일에 있다
        try:
            self.icon.notify("도우미가 꺼졌습니다. 트레이 메뉴의 \"기록 보기\" 에서 이유를 볼 수 있습니다.", "LLM Deck")
        except Exception:
            pass
        self.root.after(5000, self.quit)  # 알림을 읽을 동안 기다렸다 끝낸다

    def stop_helper(self) -> None:
        if self.job:
            self.job.kill()
            self.job = None

    def restart(self) -> None:
        if self.helper is None or self.helper.poll() is not None:
            return
        self.restarting = True
        self.stop_helper()  # 끝난 것을 알면 새로 띄운다

    def toggle_top(self) -> None:
        on = read_setting("TopMost", 1) == 0
        if self.mirror:
            self.mirror.set_top(on)
        else:
            save_setting("TopMost", 1 if on else 0)
        self.icon.update_menu()

    def open_mirror(self) -> None:
        if self.mirror is None:
            self.mirror = MirrorWindow(self.root, on_closed=self.mirror_closed)
            # 창에서 항상 위를 바꾸면 트레이 메뉴의 체크도 맞춘다
            self.mirror.top_var.trace_add("write", lambda *a: self.icon.update_menu())
        self.mirror.show()

    def mirror_closed(self, which: MirrorWindow) -> None:
        if self.mirror is which:
            self.mirror = None

    def show_log(self) -> None:
        if LOG.exists():
            subprocess.Popen(["notepad.exe", str(LOG)])

    def message(self, text: str) -> None:
        from tkinter import messagebox

        messagebox.showwarning("LLM Deck", text)

    def quit(self) -> None:
        self.quitting = True
        self.stop_helper()
        if self.mirror:
            self.mirror.close()
        try:
            self.icon.stop()
        except Exception:
            pass
        self.root.destroy()


# ---- 시작 ----

def make_root():
    import tkinter as tk

    if sys.platform == "win32":
        # Windows 배율(125%, 150%)로 키우지 않고 기기 1픽셀 = 화면 1픽셀로 그린다. 작으면 2배·3배로
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            user32.SetProcessDPIAware()
    root = tk.Tk()
    root.withdraw()  # 창은 Toplevel 로 띄운다. 이 창은 보이지 않는 바탕
    return root


def run_mirror_only() -> None:
    """앱 없이 미러링 창만 (터미널에서 도우미를 띄웠을 때). 이미 떠 있으면 그 창을 앞으로 가져온다"""
    if sys.platform == "win32":
        kernel32.CreateMutexW(None, True, MIRROR_ONLY)
        if ctypes.get_last_error() == ERROR_ALREADY_EXISTS:
            hwnd = user32.FindWindowW(None, TITLE)
            if hwnd:
                if user32.IsIconic(hwnd):
                    user32.ShowWindow(hwnd, 9)  # 9 = 최소화 풀기
                user32.SetForegroundWindow(hwnd)
            return
    root = make_root()
    window = MirrorWindow(root, on_closed=lambda w: root.destroy())
    window.show()
    root.mainloop()


def write_icon(path: str) -> None:
    tray_image(256).save(path, format="ICO", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--background", action="store_true", help="로그인할 때: 미러링 창 없이 도우미만 켠다")
    parser.add_argument("--mirror", action="store_true", help="미러링 창만 연다")
    parser.add_argument("--write-icon", metavar="FILE")
    args = parser.parse_args()
    if args.write_icon:
        return write_icon(args.write_icon)
    if sys.platform != "win32":
        return run_mirror_only()  # 개발용: Mac 에서 창만 시험한다

    os.chdir(COMPANION)
    event = kernel32.CreateEventW(None, False, False, OPEN_SIGNAL)
    if ctypes.get_last_error() == ERROR_ALREADY_EXISTS:
        # 앱이 이미 떠 있다. 로그인할 때가 아니면 그 앱에 미러링 창을 열라고 알린다
        if not args.background:
            kernel32.SetEvent(event)
        return
    if args.mirror:
        # 앱이 없다 (터미널에서 도우미를 띄웠다). 신호 이름을 비워 두고 창만 띄운다
        kernel32.CloseHandle(event)
        return run_mirror_only()
    root = make_root()
    app = DeckApp(root, event)
    if not app.quitting and not args.background:
        app.open_mirror()
    root.mainloop()


if __name__ == "__main__":
    main()
