"""Wi-Fi 로 통신한다. 사용량은 HTTP 로 보내고, 마이크 소리는 UDP 로 받는다."""

import json
import socket
import struct
import threading
import urllib.error
import urllib.request
from pathlib import Path

from . import frames

SERVICE_TYPE = "_llm-deck._tcp.local."
HOSTNAME = "llm-deck.local"
UDP_PORT = 4010
VIDEO_TCP_PORT = 4011
HEADER = struct.Struct("<I")
AUDIO_SIZE = HEADER.size + 320 * 2  # 20ms 분량
# 기기 찾기(mDNS)가 가끔 실패하므로 마지막으로 연결된 주소를 기억해 둔다
LAST_ADDRESS = Path.home() / ".llm-deck" / "device-address"


class WifiTransport:
    name = "Wi-Fi"

    def __init__(self, cfg: dict, on_audio, on_event=None, on_photo=None, on_video=None):
        self.host = cfg.get("host", "")
        self.on_audio = on_audio
        self.on_event = on_event or (lambda event: None)
        self.on_photo = on_photo or (lambda offset, data: None)
        self.on_video = on_video or (lambda no, offset, total, ms, data: None)
        self.server = None
        self.address = None
        self.sock = None

    def _candidates(self):
        """기기 주소 후보. 설정에 적힌 주소 > 지난번에 연결된 주소 > mDNS 서비스 검색 > llm-deck.local 순."""
        if self.host:
            yield self.host
            return
        try:
            yield LAST_ADDRESS.read_text().strip()
        except OSError:
            pass

        from zeroconf import Zeroconf

        zc = Zeroconf()
        try:
            info = zc.get_service_info(SERVICE_TYPE, f"llm-deck.{SERVICE_TYPE}", timeout=5000)
            if info and info.parsed_addresses():
                yield info.parsed_addresses()[0]
        finally:
            zc.close()
        try:
            yield socket.gethostbyname(HOSTNAME)
        except OSError:
            pass

    @staticmethod
    def _is_device(address: str) -> bool:
        # 엉뚱한 서버가 대답할 수도 있으므로 기기의 응답 형식인지 확인한다
        try:
            with urllib.request.urlopen(f"http://{address}/status", timeout=3) as res:
                return "mic" in json.load(res)
        except (urllib.error.URLError, OSError, ValueError, TypeError):
            return False

    def open(self) -> bool:
        address = next((a for a in self._candidates() if a and self._is_device(a)), None)
        if not address:
            return False
        self.address = address
        try:
            LAST_ADDRESS.parent.mkdir(exist_ok=True)
            LAST_ADDRESS.write_text(address)
        except OSError:
            pass

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", UDP_PORT))
        self.sock = sock
        threading.Thread(target=self._receive, args=(sock,), daemon=True).start()

        # 동영상은 양이 많아 기기가 TCP 로 접속해 보낸다
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", VIDEO_TCP_PORT))
        server.listen(1)
        self.server = server
        threading.Thread(target=self._accept, args=(server,), daemon=True).start()
        return True

    def _accept(self, server) -> None:
        while True:
            try:
                conn, _ = server.accept()
            except OSError:
                return
            threading.Thread(target=self._read_stream, args=(conn,), daemon=True).start()

    def _read_stream(self, conn) -> None:
        buf = b""
        with conn:
            while True:
                try:
                    data = conn.recv(65536)
                except OSError:
                    return
                if not data:
                    return
                found, buf = frames.split(buf + data)
                for kind, body in found:
                    frames.dispatch(self, kind, body)

    def _receive(self, sock) -> None:
        while True:
            try:
                packet = sock.recv(2048)
            except OSError:
                return
            # 같은 포트로 알림(JSON 글자)도 온다. 소리 묶음과 길이가 달라서 구별된다
            if packet[:1] == b"{" and len(packet) != AUDIO_SIZE:
                try:
                    event = json.loads(packet)
                except ValueError:
                    continue
                self.on_event(event)
                photo = event.get("photo") if isinstance(event, dict) else None
                if isinstance(photo, dict) and photo.get("url"):
                    threading.Thread(target=self._fetch_photo, args=(photo,), daemon=True).start()
                continue
            (seq,) = HEADER.unpack_from(packet)
            self.on_audio(seq, packet[HEADER.size :])

    def _fetch_photo(self, photo: dict) -> None:
        """Wi-Fi 는 사진을 조각으로 보내지 않고, 알림을 받은 PC 가 기기에서 받아 간다."""
        try:
            with urllib.request.urlopen(f"http://{self.address}{photo['url']}", timeout=20) as res:
                data = res.read()
        except (urllib.error.URLError, OSError) as e:
            print(f"[photo] Wi-Fi 로 받지 못했습니다: {e}")
            return
        self.on_event({"photo": {"name": photo.get("name", "photo.jpg"), "size": len(data)}})
        self.on_photo(0, data)

    def send(self, message: dict) -> None:
        req = urllib.request.Request(
            f"http://{self.address}/usage",
            data=json.dumps(message).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=5):
            pass

    def close(self) -> None:
        if self.server:
            self.server.close()
            self.server = None
        if self.sock:
            self.sock.close()
            self.sock = None
