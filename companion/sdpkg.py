"""sd_package.py 가 준비한 dist/LLM-Deck-Helper/ 를 USB 로 이어진 기기 SD 카드의 같은 폴더에 넣는다.

기기 카드의 version.txt 가 준비한 것과 다르면 파일을 조각내 보내고 version.txt 는 맨 마지막에 보낸다
(중간에 끊기면 다음 연결 때 다시 보낸다). 준비한 폴더가 없는 PC(설치한 도우미)는 아무것도 하지 않는다.
주고받는 내용은 firmware/src/helper_pkg.h.
"""

import base64
import queue
import threading
import time
from pathlib import Path

STAGE = Path(__file__).resolve().parents[1] / "dist" / "LLM-Deck-Helper"
CHUNK = 6144  # 기기 HELPER_PKG_CHUNK
CHECK_SECONDS = 60  # sd_package.py 를 다시 돌렸는지 이만큼마다 본다
REPLY_SECONDS = 10


class SdPackage:
    def __init__(self, stage: Path | None = None):
        self.stage = stage or STAGE
        self.replies: queue.Queue = queue.Queue()
        self.busy = False
        self.checked = 0.0
        self.done = None  # (연결, version) 카드에 넣었거나 같은 것을 확인한 것
        self.warned = None  # 카드가 없다고 기록한 연결

    def on_event(self, event: dict) -> None:
        if isinstance(event, dict) and isinstance(event.get("pkg"), dict):
            self.replies.put(event["pkg"])

    def poll(self, transport) -> None:
        """메인 루프가 부른다. USB 연결이고 준비한 것이 카드와 다를 수 있으면 따로 돌려 보낸다"""
        if self.busy or getattr(transport, "name", "") != "USB" or time.time() - self.checked < CHECK_SECONDS:
            return
        self.checked = time.time()
        try:
            version = (self.stage / "version.txt").read_text().strip()
        except OSError:
            return
        if self.done == (transport, version):
            return
        self.busy = True
        threading.Thread(target=self._run, args=(transport, version), daemon=True, name="sdpkg").start()

    def _ask(self, transport, pkg: dict) -> dict:
        while not self.replies.empty():
            self.replies.get_nowait()
        transport.send({"pkg": pkg})
        return self.replies.get(timeout=REPLY_SECONDS)

    def _send_file(self, transport, path: Path) -> None:
        data = path.read_bytes()
        off = 0
        for _ in range(len(data) // CHUNK + 20):  # 기기가 받아 둔 자리로 몇 번 되돌아가도 끝나게
            piece = data[off : off + CHUNK]
            done = off + len(piece) >= len(data)
            reply = self._ask(transport, {"name": path.name, "off": off, "data": base64.b64encode(piece).decode(), "done": done})
            if "error" in reply:
                raise OSError(f"{path.name}: {reply['error']}")
            if done and reply.get("off") == len(data):
                return
            off = int(reply.get("off", 0))
        raise OSError(f"{path.name}: 기기가 받은 자리가 맞지 않습니다")

    def _run(self, transport, version: str) -> None:
        try:
            try:
                reply = self._ask(transport, {"ask": True})
            except queue.Empty:
                self.done = (transport, version)  # 이걸 모르는 옛 펌웨어. 다음 연결이나 새 준비 때 다시 묻는다
                return
            if not reply.get("card"):
                # 카드가 없거나 읽을 수 없거나 USB 드라이브로 빌려준 중. 다음 확인 때 다시 (기록은 연결마다 한 번)
                if self.warned is not transport:
                    self.warned = transport
                    print("[sd-pkg] 기기 SD 카드를 읽을 수 없어 설치 파일을 넣지 못했습니다. 1분마다 다시 봅니다")
                return
            if reply.get("ver") == version:
                self.done = (transport, version)
                return
            started = time.time()
            files = sorted(p for p in self.stage.iterdir() if p.is_file() and p.name != "version.txt")
            for path in files + [self.stage / "version.txt"]:
                self._send_file(transport, path)
            self.done = (transport, version)
            size = sum(p.stat().st_size for p in files) // 1024
            print(f"[sd-pkg] 기기 SD 카드에 설치 파일을 넣었습니다 ({version}, {size}KB, {time.time() - started:.0f}초)")
        except (OSError, queue.Empty) as e:
            print(f"[sd-pkg] 기기 SD 카드에 넣지 못했습니다: {e or '기기 답 없음'}. 다음에 다시 합니다")
        finally:
            self.busy = False
