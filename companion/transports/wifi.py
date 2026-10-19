"""Wi-Fi 로 통신한다. 사용량은 HTTP 로 보내고, 마이크 소리는 UDP 로 받는다."""

import hmac
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

from . import auth, frames

SERVICE_TYPE = "_llm-deck._tcp.local."
HOSTNAME = "llm-deck.local"
UDP_PORT = 4010
VIDEO_TCP_PORT = 4011
HEADER = struct.Struct("<I")
AUDIO_SIZE = HEADER.size + 320 * 2  # 20ms 분량
# 기기 찾기(mDNS)가 가끔 실패하므로 마지막으로 연결된 주소를 기억해 둔다
LAST_ADDRESS = Path.home() / ".llm-deck" / "device-address"
# 같은 안내를 다시 적기 전에 기다리는 시간 (연결은 몇 초마다 다시 시도한다)
NOTICE_GAP = 600
_noticed: dict[str, float] = {}


def _notice(text: str) -> None:
    now = time.monotonic()
    if now - _noticed.get(text, -NOTICE_GAP) >= NOTICE_GAP:
        _noticed[text] = now
        print(text)


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
        # 인증 (auth.py). 기기가 /status 로 준 nonce 와 보낸 순번. UDP 는 바로 앞 nonce 로 서명된 것도 받는다
        self.device_id = ""
        self.key = None
        self.nonce = b""
        self.ctr = 0
        self.rx = []  # [(nonce, auth.Window)] 최근 것이 앞
        self._auth_lock = threading.Lock()

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
    def _status(address: str) -> dict | None:
        # 엉뚱한 서버가 대답할 수도 있으므로 기기의 응답 형식인지 확인한다
        try:
            with urllib.request.urlopen(f"http://{address}/status", timeout=3) as res:
                info = json.load(res)
            return info if isinstance(info, dict) and "mic" in info else None
        except (urllib.error.URLError, OSError, ValueError, TypeError):
            return None

    def _use_status(self, info: dict) -> bool:
        """/status 응답에서 이 기기의 키와 새 nonce 를 고른다. 키가 없으면 False."""
        if info.get("auth") != 1:
            _notice("[Wi-Fi] 기기 펌웨어가 Wi-Fi 인증을 모르는 옛 버전이라 Wi-Fi 를 쓰지 않습니다. 펌웨어를 업데이트하세요.")
            return False
        device_id = str(info.get("id", ""))
        key = auth.load_key(device_id)
        if not key:
            _notice(f"[Wi-Fi] 이 기기({device_id})의 인증 키가 없습니다. {auth.NEED_USB}")
            return False
        try:
            nonce = bytes.fromhex(info.get("nonce", ""))
        except (ValueError, TypeError):
            return False
        if len(nonce) != 16:
            return False
        with self._auth_lock:
            self.device_id, self.key, self.nonce, self.ctr = device_id, key, nonce, 0
            self.rx = [(nonce, auth.Window())] + self.rx[:1]
        return True

    def open(self) -> bool:
        address = None
        for candidate in self._candidates():
            info = candidate and self._status(candidate)
            if info:
                address = candidate
                break
        if not address or not self._use_status(info):
            return False
        try:
            # 설정에 이름(llm-deck.local)을 적었어도 받는 쪽에서 보낸 곳과 맞춰 보려면 숫자 주소가 필요하다
            address = socket.gethostbyname(address)
        except OSError:
            pass
        # 받는 포트(UDP 4010, TCP 4011)는 같은 네트워크의 누구나 보낼 수 있다. 기기 알림은 입력·앱 실행·마이크를
        # 움직이므로, 위에서 /status 로 기기임을 확인한 이 주소에서 온 것만 받는다. 기기의 주소는 받은 묶음에서
        # 배우지 않는다 (보낸 곳 주소는 아무나 꾸밀 수 있다). 기기 주소가 바뀌면 보내기가 실패해 다시 찾는다
        self.address = address
        try:
            LAST_ADDRESS.parent.mkdir(exist_ok=True)
            LAST_ADDRESS.write_text(address)
        except OSError:
            pass

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # Windows 에서 SO_REUSEADDR 는 다른 프로그램이 같은 포트를 함께 잡게 해 버리므로 Mac/Linux 에서만 쓴다
        if sys.platform != "win32":
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("0.0.0.0", UDP_PORT))
            # 동영상은 양이 많아 기기가 TCP 로 접속해 보낸다
            server.bind(("0.0.0.0", VIDEO_TCP_PORT))
            server.listen(1)
        except OSError as e:
            sock.close()
            server.close()
            print(f"[Wi-Fi] 포트 {UDP_PORT}/{VIDEO_TCP_PORT} 를 다른 프로그램이 쓰고 있어 Wi-Fi 연결을 건너뜁니다 ({e})")
            return False
        self.sock = sock
        threading.Thread(target=self._receive, args=(sock,), daemon=True).start()
        self.server = server
        threading.Thread(target=self._accept, args=(server,), daemon=True).start()
        return True

    def _accept(self, server) -> None:
        while True:
            try:
                conn, peer = server.accept()
            except OSError:
                return
            if peer[0] != self.address:
                conn.close()  # 기기가 아닌 곳에서 접속했다 (open 의 설명 참고)
                continue
            threading.Thread(target=self._read_stream, args=(conn,), daemon=True).start()

    def _tcp_hello(self, conn) -> bytes | None:
        """도전값을 보내고 기기의 첫 묶음(서명)을 확인한다 (auth.py). 맞으면 그 뒤에 이어 온 바이트를 돌려준다."""
        challenge = os.urandom(16)
        buf = b""
        try:
            conn.settimeout(5)
            conn.sendall(challenge)
            while len(buf) < 5 + 32:
                data = conn.recv(4096)
                if not data:
                    return None
                buf += data
            conn.settimeout(None)
        except OSError:
            return None
        key = self.key
        if (buf[:5] != b"\xa5\x5a" + bytes([auth.FRAME_AUTH, 32, 0]) or not key
                or not hmac.compare_digest(buf[5:37], auth.tcp_tag(key, challenge))):
            _notice(f"[Wi-Fi] 서명이 맞지 않는 TCP 접속을 끊었습니다. 기기 키가 바뀌었다면 {auth.NEED_USB}")
            return None
        return buf[37:]

    def _read_stream(self, conn) -> None:
        with conn:
            data = self._tcp_hello(conn)  # 서명 뒤에 같이 온 바이트부터
            if data is None:
                return
            buf = b""
            while data:
                found, buf = frames.split(buf + data)
                for kind, body in found:
                    frames.dispatch(self, kind, body)
                try:
                    data = conn.recv(65536)
                except OSError:
                    return

    def _receive(self, sock) -> None:
        while True:
            try:
                packet, sender = sock.recvfrom(2048)
            except OSError:
                return
            if sender[0] != self.address:
                continue  # 기기가 아닌 곳에서 온 것 (open 의 설명 참고)
            packet = self._verify(packet)
            if packet is None:
                continue  # 서명이 틀렸거나 이미 받은 것 (보낸 곳 주소는 꾸밀 수 있다)
            try:
                self._on_packet(packet)
            except Exception:
                frames.report(self.name)

    def _verify(self, packet: bytes) -> bytes | None:
        key = self.key
        if not key:
            return None
        with self._auth_lock:
            for nonce, window in self.rx:
                opened = auth.udp_open(key, nonce, packet)
                if opened:
                    return opened[1] if window.accept(opened[0]) else None
        return None

    def _on_packet(self, packet: bytes) -> None:
        # 같은 포트로 알림(JSON 글자)도 온다. 소리 묶음과 길이가 달라서 구별된다
        if packet[:1] == b"{" and len(packet) != AUDIO_SIZE:
            try:
                event = json.loads(packet)
            except ValueError:
                return
            self.on_event(event)
            photo = event.get("photo") if isinstance(event, dict) else None
            if isinstance(photo, dict) and photo.get("url"):
                threading.Thread(target=self._fetch_photo, args=(photo,), daemon=True).start()
            return
        if len(packet) < HEADER.size:
            return
        (seq,) = HEADER.unpack_from(packet)
        self.on_audio(seq, packet[HEADER.size :])

    def _fetch_photo(self, photo: dict) -> None:
        """Wi-Fi 는 사진을 조각으로 보내지 않고, 알림을 받은 PC 가 기기에서 받아 간다."""
        try:
            data = self._request(str(photo["url"]), timeout=20)
        except (urllib.error.URLError, OSError) as e:
            print(f"[photo] Wi-Fi 로 받지 못했습니다: {e}")
            return
        self.on_event({"photo": {"name": photo.get("name", "photo.jpg"), "size": len(data)}})
        self.on_photo(0, data)

    def connected(self) -> bool:
        # Wi-Fi 는 연결을 유지하지 않고 보낼 때마다 요청하므로, 끊김은 보낼 때 알게 된다
        return True

    def _request(self, path: str, body: bytes | None = None, timeout: float = 5) -> bytes:
        """서명을 붙여 요청한다. 기기가 다시 켜져 nonce 가 낡았으면 새로 받아 한 번 더 보낸다."""
        for attempt in range(2):
            with self._auth_lock:
                self.ctr += 1
                header = auth.http_header(self.key, self.nonce, self.ctr, path, body or b"")
            req = urllib.request.Request(
                f"http://{self.address}{path}",
                data=body,
                headers={"Content-Type": "application/json", "X-Deck-Auth": header},
                method="POST" if body is not None else "GET",
            )
            try:
                with urllib.request.urlopen(req, timeout=timeout) as res:
                    return res.read()
            except urllib.error.HTTPError as e:
                if e.code != 401:
                    raise
                try:
                    reason = json.loads(e.read()).get("auth")
                except (ValueError, AttributeError, OSError):
                    reason = ""
                if reason == "stale" and attempt == 0:
                    info = self._status(self.address)
                    if info and str(info.get("id", "")) == self.device_id and self._use_status(info):
                        continue
                if reason != "stale":
                    _notice(f"[Wi-Fi] 기기가 인증을 거절했습니다 ({reason}). {auth.NEED_USB}")
                raise OSError(f"Wi-Fi 인증 실패 ({reason})") from e
        raise OSError("Wi-Fi 인증 실패")

    def send(self, message: dict) -> None:
        self._request("/usage", json.dumps(message).encode())

    def close(self) -> None:
        if self.server:
            self.server.close()
            self.server = None
        if self.sock:
            self.sock.close()
            self.sock = None
