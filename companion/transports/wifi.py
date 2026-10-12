"""Wi-Fi 로 통신한다. 사용량은 HTTP 로 보내고, 마이크 소리는 UDP 로 받는다."""

import json
import socket
import struct
import threading
import urllib.error
import urllib.request

SERVICE_TYPE = "_llm-deck._tcp.local."
HOSTNAME = "llm-deck.local"
UDP_PORT = 4010
HEADER = struct.Struct("<I")


class WifiTransport:
    name = "Wi-Fi"

    def __init__(self, cfg: dict, on_audio):
        self.host = cfg.get("host", "")
        self.on_audio = on_audio
        self.address = None
        self.sock = None

    def _find(self) -> str | None:
        """기기의 IP 를 찾는다. 설정에 적힌 주소 > mDNS 서비스 검색 > llm-deck.local 순."""
        if self.host:
            return self.host

        from zeroconf import Zeroconf

        zc = Zeroconf()
        try:
            info = zc.get_service_info(SERVICE_TYPE, f"llm-deck.{SERVICE_TYPE}", timeout=3000)
            if info and info.parsed_addresses():
                return info.parsed_addresses()[0]
        finally:
            zc.close()
        try:
            return socket.gethostbyname(HOSTNAME)
        except OSError:
            return None

    def open(self) -> bool:
        address = self._find()
        if not address:
            return False
        try:
            with urllib.request.urlopen(f"http://{address}/status", timeout=3):
                pass
        except (urllib.error.URLError, OSError):
            return False
        self.address = address

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", UDP_PORT))
        self.sock = sock
        threading.Thread(target=self._receive, args=(sock,), daemon=True).start()
        return True

    def _receive(self, sock) -> None:
        while True:
            try:
                packet = sock.recv(2048)
            except OSError:
                return
            (seq,) = HEADER.unpack_from(packet)
            self.on_audio(seq, packet[HEADER.size :])

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
        if self.sock:
            self.sock.close()
            self.sock = None
