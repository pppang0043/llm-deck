"""기기 화면 미러링. 이 PC 의 브라우저에서 기기 화면을 보고, 눌러서 기기를 조작한다 (USB 나 Wi-Fi 로 연결했을 때).

도우미가 이 PC 안에서만 열리는 작은 웹 서버를 띄운다. 주소는 늘 http://127.0.0.1:4013/ 이라 즐겨찾기해 두면 되고,
`uv run main.py --mirror` 가 그 주소를 브라우저로 연다. 다른 웹 페이지가 몰래 기기를 누르지 못하게,
브라우저가 붙여 보내는 "어디서 온 요청인지"(Origin, Sec-Fetch-Site)를 보고 이 페이지에서 온 것만 받는다.

창이 열려 있는 동안(페이지가 화면을 받아 가는 동안)만 기기에 {"mirror": true} 를 2초마다 보낸다.
기기는 화면이 바뀔 때만 한 장을 보낸다 (펌웨어 mirror.h). 같은 색이 이어지는 것만 줄인 RGB565 라서 브라우저가 풀어 그린다. 창에서 누르기·끌기·떼기는 {"touch": {...}} 로,
휠·키보드로 밀기는 누르기부터 떼기까지의 점들을 {"touch": [...]} 한 번에 보낸다. Esc 는 {"view": "home"}.
"""

import json
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PORT = 4013  # 4012 는 도우미가 하나만 돌게 잡는 포트 (main.py)
URL = f"http://127.0.0.1:{PORT}/"
OLD_URL_FILE = Path.home() / ".llm-deck" / "mirror_url.txt"  # 예전에 주소가 바뀌던 때 쓰던 파일
# 브라우저 대신 쓰는 미러링 창 (항상 위). Mac 은 도우미 앱(macos/app.swift)을 한 번 더 열면 창이 뜬다
MAC_APP = Path.home() / "Applications" / "LLM Deck.app"
WIN_APP = Path(__file__).resolve().parent / "windows" / "app.pyw"  # Windows 도우미 앱. --mirror 로 창만 연다
WIN_OPEN_SIGNAL = "LLMDeckOpenMirror"  # 떠 있는 Windows 도우미 앱에 창을 열라고 알리는 신호 (app.pyw)
HEAD = struct.Struct("<IIIHH")  # 장 번호, 위치, 장 크기, 폭, 높이
# 페이지가 이만큼 화면을 받아 가지 않으면 창이 닫힌 것으로 본다
VIEWER_SECONDS = 3
# 이보다 오래 끊겼다가 다시 붙으면 새로 연결한 것으로 보고 미러링 창을 띄운다 (잠깐 끊겼다 붙은 것은 그대로 둔다)
REJOIN_SECONDS = 60
KEEP_SECONDS = 2
# 화면이 그대로여도 상태줄 시계가 1분마다 바뀌어 한 장은 오고, USB 면 배터리 기록도 1분마다 온다.
# 미러링 중 이만큼 기기에서 아무것도 오지 않으면 기기가 멈춘 것으로 보고 창에 알린다
# (펌웨어 올리기가 중간에 끊겨 기기가 반쯤 써진 채 멈췄을 때 창이 마지막 화면 그대로 먹통처럼 보였다)
SILENT_SECONDS = 75

PAGE = """<!doctype html>
<html lang="ko"><head><meta charset="utf-8"><title>LLM Deck 미러링</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  :root { color-scheme: dark; }
  body { -webkit-user-select: none; user-select: none; margin: 0; background: #0b0e11; color: #8a939e; font: 14px -apple-system, "Malgun Gothic", sans-serif;
         display: flex; flex-direction: column; align-items: center; gap: 10px; padding: 16px; }
  /* 그림(img)은 끌면 브라우저가 그림을 옮기려 하므로 캔버스에 그린다 */
  #screen { image-rendering: pixelated; border-radius: 10px; box-shadow: 0 0 0 1px #2a323b; cursor: pointer;
            touch-action: none; user-select: none; -webkit-user-drag: none; }
  .on { background: #3a4654; }
  #state { min-height: 1.4em; }
  button { background: #1c232b; color: #fff; border: 0; border-radius: 6px; padding: 6px 10px; cursor: pointer; }
</style></head>
<body>
  <canvas id="screen" width="320" height="240"></canvas>
  <div id="state">기기 화면을 기다리는 중...</div>
  <div id="zooms"><button data-zoom="0">맞춤</button> <button data-zoom="1">1배</button> <button data-zoom="2">2배</button> <button data-zoom="3">3배</button></div>
  <div id="help">휠·↑↓ 위아래로 밀기 · ←→ 좌우로 밀기 · Esc 홈</div>
<script>
const img = document.getElementById("screen"), ctx = img.getContext("2d"), state = document.getElementById("state");
// 0 = 맞춤: 화면 전체가 창 안에 들어오게 키우거나 줄인다 (창 크기를 바꾸면 따라간다). 1~3 = 배율
let zoom = 0, seq = 0, w = 0, h = 0, down = false, lastMove = 0;
// 예전("zoom")에는 맞춤이 없어 2배가 기본이었다. 새 이름으로 저장해 처음에는 맞춤으로 연다
try { const saved = localStorage.getItem("mirrorZoom"); if (saved !== null) zoom = +saved || 0; } catch (e) {}
function size() {
  const sw = w || 320, sh = h || 240;
  let scale = zoom;
  if (!zoom) {
    // 창 높이에서 아래 글자·단추 줄과 여백(위아래 16, 줄 사이 10)을 뺀 만큼
    const rest = ["state", "zooms", "help"].reduce((sum, id) => sum + document.getElementById(id).offsetHeight + 10, 32);
    scale = Math.max(0.25, Math.min((innerWidth - 32) / sw, (innerHeight - rest) / sh));
  }
  img.style.width = Math.floor(sw * scale) + "px";
  document.querySelectorAll("[data-zoom]").forEach(b => b.classList.toggle("on", +b.dataset.zoom === zoom));
}
document.querySelectorAll("[data-zoom]").forEach(b => b.onclick = () => {
  zoom = +b.dataset.zoom; size(); try { localStorage.setItem("mirrorZoom", zoom); } catch (e) {}
});
addEventListener("resize", size);
size();
// 기기가 줄인 화면을 푼다 (펌웨어 mirror.cpp 의 pack): 머리 h < 128 이면 h+1 점 그대로, 아니면 한 점을 h-126 번
const R = new Uint8Array(32), G = new Uint8Array(64);
for (let i = 0; i < 32; i++) R[i] = Math.round(i * 255 / 31);
for (let i = 0; i < 64; i++) G[i] = Math.round(i * 255 / 63);
function draw(src) {
  const pic = ctx.createImageData(w, h), d = new Uint32Array(pic.data.buffer), n = w * h;
  const color = v => 0xff000000 | (R[v & 31] << 16) | (G[(v >> 5) & 63] << 8) | R[v >> 11];  // 작은 끝: ABGR
  let o = 0, p = 0;
  while (o < src.length && p < n) {
    const head = src[o++];
    if (head < 128) {
      for (let i = 0; i <= head && p < n; i++, o += 2) d[p++] = color(src[o] | (src[o + 1] << 8));
    } else {
      const c = color(src[o] | (src[o + 1] << 8));
      o += 2;
      for (let i = 0; i < head - 126 && p < n; i++) d[p++] = c;
    }
  }
  ctx.putImageData(pic, 0, 0);
}
async function poll() {
  for (;;) {
    try {
      const r = await fetch("frame?after=" + seq, {cache: "no-store"});
      if (r.status === 200) {
        seq = +r.headers.get("X-Seq"); w = +r.headers.get("X-Width"); h = +r.headers.get("X-Height");
        if (img.width !== w || img.height !== h) { img.width = w; img.height = h; }
        draw(new Uint8Array(await r.arrayBuffer()));
        size(); state.textContent = "누르거나 끌어서 기기를 조작합니다";
      } else if (r.status === 503) {
        state.textContent = await r.text();
      }
    } catch (e) {
      state.textContent = "도우미와 연결이 끊겼습니다. 도우미가 켜져 있는지 확인하세요";
      await new Promise(ok => setTimeout(ok, 2000));
    }
  }
}
function point(e) {
  const r = img.getBoundingClientRect();
  return {x: Math.floor((e.clientX - r.left) * w / r.width), y: Math.floor((e.clientY - r.top) * h / r.height)};
}
function post(path, body) {
  return fetch(path, {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(body)});
}
// 밀기: 화면 가운데를 지나 한쪽에서 다른 쪽으로 끄는 점들 (기기가 10ms 마다 하나씩 쓴다)
function swipe(dx, dy) {
  if (!w) return;
  const x0 = w / 2 - dx * w * 0.35, y0 = h / 2 - dy * h * 0.3, points = [];
  for (let i = 0; i <= 8; i++) {
    points.push({x: Math.round(x0 + dx * w * 0.7 * i / 8), y: Math.round(y0 + dy * h * 0.6 * i / 8), down: true});
  }
  points.push({...points[points.length - 1], down: false});
  post("touch", {points});
}
let lastSwipe = 0, wheel = 0;
img.addEventListener("wheel", e => {
  e.preventDefault();
  wheel += e.deltaY;
  if (Math.abs(wheel) < 40 || e.timeStamp - lastSwipe < 350) return;
  swipe(0, wheel > 0 ? -1 : 1);  // 아래로 굴리면 다음 쪽 (손가락을 위로 미는 것)
  wheel = 0; lastSwipe = e.timeStamp;
}, {passive: false});
document.addEventListener("keydown", e => {
  const moves = {ArrowDown: [0, -1], ArrowUp: [0, 1], ArrowRight: [-1, 0], ArrowLeft: [1, 0]};
  if (moves[e.key]) { e.preventDefault(); if (!e.repeat) swipe(...moves[e.key]); }
  else if (e.key === "Escape") post("home", {});
});
function send(e, isDown) {
  if (!w) return;
  const p = point(e);
  post("touch", {x: p.x, y: p.y, down: isDown});
}
// 브라우저 기본 동작(끌기, 글자 선택, 오른쪽 메뉴)은 막는다
for (const kind of ["dragstart", "selectstart", "contextmenu"]) img.addEventListener(kind, e => e.preventDefault());
img.addEventListener("pointerdown", e => { e.preventDefault(); down = true; img.setPointerCapture(e.pointerId); send(e, true); });
img.addEventListener("pointermove", e => {
  if (!down || e.timeStamp - lastMove < 30) return;
  lastMove = e.timeStamp; send(e, true);
});
img.addEventListener("pointerup", e => { if (down) { down = false; send(e, false); } });
img.addEventListener("pointercancel", e => { if (down) { down = false; send(e, false); } });
poll();
</script></body></html>
"""


class Mirror:
    def __init__(self, cfg: dict, get_transport):
        self.get_transport = get_transport
        self.cond = threading.Condition()
        # 누르기를 보낸 차례대로 기기에 닿게 한다 (Wi-Fi 는 보낼 때마다 따로 요청한다)
        self.send_lock = threading.Lock()
        self.frame = b""
        self.size = (0, 0)
        self.seq = 0
        self.last_poll = 0.0
        self.app_poll = 0.0  # 브라우저가 아닌 미러링 앱 창이 마지막으로 화면을 받아 간 시각
        self.sent_on = 0.0  # 마지막으로 {"mirror": true} 를 보낸 시각 (0 = 꺼짐)
        self.alive_at = 0.0  # 미러링을 켠 시각이나 마지막으로 화면을 받은 시각
        self._buf = None
        self._no = -1
        self._got = 0
        self.link = ""  # 붙어 있는(또는 마지막으로 붙었던) 연결 이름. "" = 도우미를 켜고 아직 못 붙음
        self.lost_at = 0.0  # 연결이 끊긴 시각 (0 = 붙어 있음)
        self.auto_open = False
        if not cfg.get("enabled", True):
            return
        try:
            server = ThreadingHTTPServer(("127.0.0.1", PORT), self._handler())
        except OSError as e:
            print(f"[mirror] {PORT} 번 포트를 열지 못해 미러링을 쓸 수 없습니다: {e}")
            return
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        OLD_URL_FILE.unlink(missing_ok=True)
        print(f"[mirror] 미러링 창: {URL} (uv run main.py --mirror 로도 열린다)")
        self.auto_open = cfg.get("auto_open", True)

    def link_lost(self) -> None:
        if self.link and not self.lost_at:
            self.lost_at = time.time()

    def link_up(self, name: str) -> None:
        """기기와 새로 연결되면 미러링 창을 띄운다. 블루투스로는 화면이 오지 않아 띄우지 않고,
        창이 이미 떠 있거나 잠깐 끊겼다 다시 붙은 것이면 그대로 둔다 (그때마다 창이 앞으로 튀어나오지 않게)"""
        before, lost_at = self.link, self.lost_at
        self.link, self.lost_at = name, 0.0
        # 브라우저 탭이 받아 가고 있는 것은 치지 않는다 (숨은 탭 때문에 앱 창이 안 뜨지 않게)
        if not self.auto_open or name == "Bluetooth" or self.app_watching():
            return
        if (before in ("", "Bluetooth")  # 처음 붙었거나, 화면을 못 받던 블루투스에서 바뀜
                or (name == "USB" and before != "USB")  # USB 케이블을 꽂음
                or (lost_at and time.time() - lost_at >= REJOIN_SECONDS)):
            print(f"[mirror] 기기가 {name} 로 연결되어 미러링 창을 엽니다.")
            threading.Thread(target=self._open_window, daemon=True).start()

    def _open_window(self) -> None:
        # 도우미 앱이 막 켜진 참이면 (로그인 직후, 다시 시작) 여는 요청을 놓칠 수 있어 창이 화면을 받아 갈 때까지 몇 번 더 연다
        for _ in range(3):
            # 저절로 열 때는 앱 창으로만 (앱 없이 터미널에서 띄운 도우미면 열지 않는다)
            if open_window(browser=False) != "app":
                return
            for _ in range(20):
                time.sleep(0.5)
                if self.app_watching():
                    return

    def on_frame(self, body: bytes) -> None:
        """기기가 보낸 화면 조각을 모은다 (transports/frames.py 가 부른다)"""
        no, offset, total, w, h = HEAD.unpack_from(body)
        data = body[HEAD.size :]
        if offset == 0:
            self._buf, self._no, self._got = bytearray(total), no, 0
        if self._buf is None or no != self._no or offset + len(data) > len(self._buf):
            return
        self._buf[offset : offset + len(data)] = data
        self._got += len(data)
        if self._got >= len(self._buf):
            with self.cond:
                self.frame, self.size = bytes(self._buf), (w, h)
                self.seq += 1
                self.alive_at = time.time()
                self.cond.notify_all()
            self._buf = None

    def watching(self) -> bool:
        return time.time() - self.last_poll < VIEWER_SECONDS

    def app_watching(self) -> bool:
        return time.time() - self.app_poll < VIEWER_SECONDS

    def pending(self) -> dict | None:
        """메인 루프가 0.5초마다 묻는다: 기기에 보낼 미러링 켜기·끄기"""
        if self.watching():
            if time.time() - self.sent_on >= KEEP_SECONDS:
                if not self.sent_on:
                    self.alive_at = time.time()
                self.sent_on = time.time()
                return {"mirror": True}
        elif self.sent_on:
            self.sent_on = 0.0
            return {"mirror": False}
        return None

    def silent(self) -> bool:
        """미러링 중인데 기기에서 화면도 (USB 면) 다른 것도 오래 오지 않는다"""
        if not self.sent_on:
            return False
        transport = self._link()
        heard = max(self.alive_at, getattr(transport, "last_rx", 0.0))
        return time.time() - heard > SILENT_SECONDS

    def _link(self):
        """미러링에 쓸 연결 (블루투스는 너무 느려서 쓰지 않는다)"""
        transport = self.get_transport()
        return transport if transport and transport.name in ("USB", "Wi-Fi") else None

    def _handler(self):
        mirror = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args) -> None:
                pass

            def _allowed(self) -> str | None:
                """요청 경로 ("/" 뒤). 다른 곳에서 온 요청이면 None"""
                hosts = (f"127.0.0.1:{PORT}", f"localhost:{PORT}")
                # 다른 사이트가 이름(DNS)을 바꿔 이 서버에 붙는 것을 막는다
                if self.headers.get("Host", "") not in hosts:
                    return None
                # 다른 웹 페이지가 보낸 요청은 브라우저가 출처를 붙여 보낸다. 주소창에서 연 것과 이 페이지에서 온 것만 받는다
                origin = self.headers.get("Origin")
                if origin and origin not in [f"http://{h}" for h in hosts]:
                    return None
                if self.headers.get("Sec-Fetch-Site", "none") not in ("none", "same-origin"):
                    return None
                if not self.path.startswith("/"):
                    return None
                return self.path[1:]

            def _reply(self, code: int, body: bytes, kind: str, extra: dict | None = None) -> None:
                try:
                    self.send_response(code)
                    self.send_header("Content-Type", kind)
                    self.send_header("Content-Length", str(len(body)))
                    self.send_header("Cache-Control", "no-store")
                    for k, v in (extra or {}).items():
                        self.send_header(k, v)
                    self.end_headers()
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # 창을 닫았다

            def do_GET(self) -> None:
                path = self._allowed()
                if path is None:
                    self._reply(404, b"", "text/plain")
                elif path == "":
                    self._reply(200, PAGE.encode(), "text/html; charset=utf-8")
                elif path.startswith("frame"):
                    self._frame(path)
                else:
                    self._reply(404, b"", "text/plain")

            def _frame(self, path: str) -> None:
                try:
                    after = int(path.partition("after=")[2] or 0)
                except ValueError:
                    after = 0
                mirror.last_poll = time.time()
                # 미러링 앱(Mac URLSession, Windows app.pyw 의 urllib)은 브라우저처럼 Mozilla 를 붙이지 않는다
                if "Mozilla" not in self.headers.get("User-Agent", ""):
                    mirror.app_poll = mirror.last_poll
                if not mirror._link():
                    time.sleep(1)
                    self._reply(503, "기기가 USB 나 Wi-Fi 로 연결되어 있지 않습니다 (블루투스로는 미러링할 수 없습니다)".encode(),
                                "text/plain; charset=utf-8")
                    return
                # 새 장이 올 때까지 기다린다 (화면이 그대로면 기기가 보내지 않는다)
                with mirror.cond:
                    mirror.cond.wait_for(lambda: mirror.seq != after and mirror.frame, timeout=1.5)
                    frame, (w, h), seq = mirror.frame, mirror.size, mirror.seq
                if seq == after or not frame:
                    if mirror.silent():
                        self._reply(503, ("기기가 응답하지 않습니다. 기기가 멈췄거나 펌웨어 올리기가 중간에 끊겼을 수 있습니다. "
                                          "기기를 다시 켜거나 펌웨어를 다시 올리세요.").encode(), "text/plain; charset=utf-8")
                        return
                    self._reply(204, b"", "text/plain")
                    return
                self._reply(200, frame, "application/octet-stream", {"X-Seq": str(seq), "X-Width": str(w), "X-Height": str(h)})

            def do_POST(self) -> None:
                path = self._allowed()
                # JSON 으로만 받는다 (다른 사이트의 폼 전송은 이 형식을 쓸 수 없다)
                if path not in ("touch", "home") or self.headers.get("Content-Type") != "application/json":
                    self._reply(404, b"", "text/plain")
                    return
                try:
                    body = json.loads(self.rfile.read(min(int(self.headers.get("Content-Length", 0)), 2000)))
                    if path == "home":
                        message = {"view": "home"}
                    elif "points" in body:
                        # 기기는 한 번에 16개까지 쌓아 둔다
                        message = {"touch": [_point(p) for p in body["points"][:14]]}
                    else:
                        message = {"touch": _point(body)}
                except (ValueError, KeyError, TypeError, AttributeError):
                    self._reply(400, b"", "text/plain")
                    return
                transport = mirror._link()
                if transport:
                    try:
                        with mirror.send_lock:
                            transport.send(message)
                    except OSError:
                        pass
                self._reply(204, b"", "text/plain")

        return Handler


def _point(p: dict) -> dict:
    return {"x": int(p["x"]), "y": int(p["y"]), "down": bool(p["down"])}


def _signal_windows_app() -> bool:
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32")
    kernel32.OpenEventW.restype = wintypes.HANDLE
    kernel32.OpenEventW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    event = kernel32.OpenEventW(0x0002, False, WIN_OPEN_SIGNAL)  # EVENT_MODIFY_STATE
    if not event:
        return False
    try:
        return bool(kernel32.SetEvent(wintypes.HANDLE(event)))
    finally:
        kernel32.CloseHandle(wintypes.HANDLE(event))


def open_window(browser: bool = True) -> str:
    """켜져 있는 도우미의 미러링 창을 연다. 미러링 창(Mac 의 LLM Deck.app, Windows 의 windows/app.pyw)이 있으면 그것을, 없으면 브라우저로
    (browser=False 면 앱으로만). 연 곳("app", "browser")을 돌려주고, 열지 않았으면 "" """
    import os
    import signal
    import subprocess
    import sys
    import webbrowser

    import urllib.request

    try:
        urllib.request.urlopen(URL, timeout=3).close()
    except OSError:
        print("[mirror] 도우미가 켜져 있지 않거나 미러링이 꺼져 있습니다.")
        return ""
    try:
        # 도우미 앱이 띄운 도우미면 그 앱에 바로 알린다. open 으로 부르면 앱이 막 켜진 참에는 놓치고 브라우저로 넘어간다
        app_pid = int(os.environ.get("LLM_DECK_APP_PID") or 0)
        if sys.platform == "darwin" and app_pid:
            os.kill(app_pid, signal.SIGUSR1)
            return "app"
        # 앱이 떠 있지 않으면 (터미널에서 도우미를 띄웠으면) 열지 않는다. 열면 도우미를 하나 더 띄우려 한다
        mac_app_up = sys.platform == "darwin" and subprocess.run(
            ["pgrep", "-f", "LLM Deck.app/Contents/MacOS/llm-deck"], capture_output=True).returncode == 0
        if mac_app_up:
            subprocess.run(["open", str(MAC_APP)], check=True, timeout=10)
            return "app"
        if sys.platform == "win32":
            # 도우미 앱(트레이 아이콘)이 떠 있으면 그 앱에 알린다
            if _signal_windows_app():
                return "app"
            # 앱이 없으면 (터미널에서 도우미를 띄웠으면) 미러링 창만 띄운다. 이 파이썬이 끝나도 창은 남게 묶음 밖으로
            pythonw = Path(sys.executable).with_name("pythonw.exe")
            if WIN_APP.exists() and pythonw.exists():
                flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
                try:
                    subprocess.Popen([str(pythonw), str(WIN_APP), "--mirror"], creationflags=flags | subprocess.CREATE_BREAKAWAY_FROM_JOB)
                except OSError:
                    subprocess.Popen([str(pythonw), str(WIN_APP), "--mirror"], creationflags=flags)
                return "app"
    except (OSError, subprocess.SubprocessError) as e:
        print(f"[mirror] 미러링 앱을 열지 못했습니다: {e}")
    if not browser:
        return ""
    print("[mirror] 브라우저로 엽니다.")
    webbrowser.open(URL)
    return "browser"
