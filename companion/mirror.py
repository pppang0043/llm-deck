"""기기 화면 미러링. 이 PC 의 브라우저에서 기기 화면을 보고, 눌러서 기기를 조작한다 (USB 로 연결했을 때만).

도우미가 이 PC 안에서만 열리는 작은 웹 서버(127.0.0.1)를 띄운다. 주소에는 도우미를 켤 때마다 바뀌는 비밀 글자가
들어 있어서, 다른 웹 페이지가 몰래 기기를 누르지 못한다. 주소는 ~/.llm-deck/mirror_url.txt 에 적어 두고,
`uv run main.py --mirror` 가 그 주소를 브라우저로 연다.

창이 열려 있는 동안(페이지가 화면을 받아 가는 동안)만 기기에 {"mirror": true} 를 2초마다 보낸다.
기기는 화면이 바뀔 때만 JPEG 한 장을 보낸다 (펌웨어 mirror.h). 창에서 누르기·끌기·떼기는 {"touch": {...}} 로 보낸다.
"""

import json
import secrets
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PORT = 4013  # 4012 는 도우미가 하나만 돌게 잡는 포트 (main.py)
URL_FILE = Path.home() / ".llm-deck" / "mirror_url.txt"
HEAD = struct.Struct("<IIIHH")  # 장 번호, 위치, 장 크기, 폭, 높이
# 페이지가 이만큼 화면을 받아 가지 않으면 창이 닫힌 것으로 본다
VIEWER_SECONDS = 3
KEEP_SECONDS = 2

PAGE = """<!doctype html>
<html lang="ko"><head><meta charset="utf-8"><title>LLM Deck 미러링</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  :root { color-scheme: dark; }
  body { -webkit-user-select: none; user-select: none; margin: 0; background: #0b0e11; color: #8a939e; font: 14px -apple-system, "Malgun Gothic", sans-serif;
         display: flex; flex-direction: column; align-items: center; gap: 10px; padding: 16px; }
  /* 그림(img)은 끌면 브라우저가 그림을 옮기려 하므로 캔버스에 그린다 */
  #screen { image-rendering: pixelated; border-radius: 10px; box-shadow: 0 0 0 1px #2a323b; cursor: pointer;
            touch-action: none; user-select: none; -webkit-user-drag: none; max-width: calc(100vw - 32px); }
  #state { min-height: 1.4em; }
  button { background: #1c232b; color: #fff; border: 0; border-radius: 6px; padding: 6px 10px; cursor: pointer; }
</style></head>
<body>
  <canvas id="screen" width="320" height="240"></canvas>
  <div id="state">기기 화면을 기다리는 중...</div>
  <div><button data-zoom="1">1배</button> <button data-zoom="2">2배</button> <button data-zoom="3">3배</button></div>
<script>
const img = document.getElementById("screen"), ctx = img.getContext("2d"), state = document.getElementById("state");
let zoom = 2, seq = 0, w = 0, h = 0, down = false, lastMove = 0;
try { zoom = +localStorage.getItem("zoom") || 2; } catch (e) {}
function size() { if (w) img.style.width = (w * zoom) + "px"; }
document.querySelectorAll("[data-zoom]").forEach(b => b.onclick = () => {
  zoom = +b.dataset.zoom; size(); try { localStorage.setItem("zoom", zoom); } catch (e) {}
});
async function poll() {
  for (;;) {
    try {
      const r = await fetch("frame?after=" + seq, {cache: "no-store"});
      if (r.status === 200) {
        seq = +r.headers.get("X-Seq"); w = +r.headers.get("X-Width"); h = +r.headers.get("X-Height");
        const pic = await createImageBitmap(await r.blob());
        if (img.width !== w || img.height !== h) { img.width = w; img.height = h; }
        ctx.drawImage(pic, 0, 0); pic.close();
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
function send(e, isDown) {
  if (!w) return;
  const p = point(e);
  fetch("touch", {method: "POST", headers: {"Content-Type": "application/json"},
                  body: JSON.stringify({x: p.x, y: p.y, down: isDown})});
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
        self.token = secrets.token_urlsafe(16)
        self.cond = threading.Condition()
        self.frame = b""
        self.size = (0, 0)
        self.seq = 0
        self.last_poll = 0.0
        self.sent_on = 0.0  # 마지막으로 {"mirror": true} 를 보낸 시각 (0 = 꺼짐)
        self._buf = None
        self._no = -1
        self._got = 0
        if not cfg.get("enabled", True):
            return
        try:
            server = ThreadingHTTPServer(("127.0.0.1", PORT), self._handler())
        except OSError as e:
            print(f"[mirror] {PORT} 번 포트를 열지 못해 미러링을 쓸 수 없습니다: {e}")
            return
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = f"http://127.0.0.1:{PORT}/{self.token}/"
        URL_FILE.parent.mkdir(parents=True, exist_ok=True)
        URL_FILE.write_text(url, encoding="utf-8")
        print(f"[mirror] 미러링 창: uv run main.py --mirror (또는 {URL_FILE} 의 주소)")

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
                self.cond.notify_all()
            self._buf = None

    def watching(self) -> bool:
        return time.time() - self.last_poll < VIEWER_SECONDS

    def pending(self) -> dict | None:
        """메인 루프가 0.5초마다 묻는다: 기기에 보낼 미러링 켜기·끄기"""
        if self.watching():
            if time.time() - self.sent_on >= KEEP_SECONDS:
                self.sent_on = time.time()
                return {"mirror": True}
        elif self.sent_on:
            self.sent_on = 0.0
            return {"mirror": False}
        return None

    def _usb(self):
        transport = self.get_transport()
        return transport if transport and transport.name == "USB" else None

    def _handler(self):
        mirror = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args) -> None:
                pass

            def _allowed(self) -> str | None:
                """비밀 글자 뒤의 경로. 다른 곳에서 온 요청이면 None"""
                # 다른 사이트가 이름(DNS)을 바꿔 이 서버에 붙는 것을 막는다
                if self.headers.get("Host", "") not in (f"127.0.0.1:{PORT}", f"localhost:{PORT}"):
                    return None
                prefix = f"/{mirror.token}/"
                if not self.path.startswith(prefix):
                    return None
                return self.path[len(prefix) :]

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
                if not mirror._usb():
                    time.sleep(1)
                    self._reply(503, "기기가 USB 로 연결되어 있지 않습니다 (미러링은 USB 로만 됩니다)".encode(),
                                "text/plain; charset=utf-8")
                    return
                # 새 장이 올 때까지 기다린다 (화면이 그대로면 기기가 보내지 않는다)
                with mirror.cond:
                    mirror.cond.wait_for(lambda: mirror.seq != after and mirror.frame, timeout=1.5)
                    frame, (w, h), seq = mirror.frame, mirror.size, mirror.seq
                if seq == after or not frame:
                    self._reply(204, b"", "text/plain")
                    return
                self._reply(200, frame, "image/jpeg", {"X-Seq": str(seq), "X-Width": str(w), "X-Height": str(h)})

            def do_POST(self) -> None:
                path = self._allowed()
                # JSON 으로만 받는다 (다른 사이트의 폼 전송은 이 형식을 쓸 수 없다)
                if path != "touch" or self.headers.get("Content-Type") != "application/json":
                    self._reply(404, b"", "text/plain")
                    return
                try:
                    body = json.loads(self.rfile.read(min(int(self.headers.get("Content-Length", 0)), 200)))
                    touch = {"x": int(body["x"]), "y": int(body["y"]), "down": bool(body["down"])}
                except (ValueError, KeyError, TypeError):
                    self._reply(400, b"", "text/plain")
                    return
                transport = mirror._usb()
                if transport:
                    try:
                        transport.send({"touch": touch})
                    except OSError:
                        pass
                self._reply(204, b"", "text/plain")

        return Handler


def open_window() -> bool:
    """켜져 있는 도우미의 미러링 창을 브라우저로 연다"""
    import webbrowser

    if not URL_FILE.exists():
        print("[mirror] 도우미가 켜져 있지 않거나 미러링이 꺼져 있습니다.")
        return False
    webbrowser.open(URL_FILE.read_text(encoding="utf-8").strip())
    return True
