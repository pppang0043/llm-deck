"""Wi-Fi·블루투스 인증. 형식은 펌웨어의 net.h, ble_link.h 와 맞춘다.

같은 네트워크의 누구나 기기에 명령을 보내거나 기기인 척 PC 로 보낼 수 없게, USB 로 한 번 받아 간 비밀 키로 서명한다.
    PC -> 기기 HTTP: 머리글 X-Deck-Auth: <nonce 32자>.<순번>.<서명 64자>
        서명 = HMAC-SHA256(키, b"http" + nonce 16바이트 + 순번 4바이트(LE) + 경로 + b"\\n" + 내용)
    기기 -> PC UDP: [내용][순번 4바이트(LE)][HMAC-SHA256(키, b"udp" + nonce + 순번 + 내용) 앞 16바이트]
    기기 -> PC TCP: PC 가 16바이트 도전값을 보내면 기기가 첫 묶음(종류 6)으로 HMAC(키, b"tcp" + 도전값) 을 보낸다
    블루투스: 도전값을 주고받아 서로 확인한 뒤 줄·알림마다 서명한다 (ble_link.h, 아래 ble_* 함수)
키는 기기별(MAC)로 ~/.llm-deck/wifi-keys.json 에 이 사용자만 읽을 수 있게 둔다.
"""

import hashlib
import hmac
import json
import os
import struct
from pathlib import Path

KEYS_FILE = Path.home() / ".llm-deck" / "wifi-keys.json"
FRAME_AUTH = 6
UDP_TRAILER = 20
NEED_USB = "USB로 한 번 연결하면 Wi-Fi를 쓸 수 있습니다"
_CTR = struct.Struct("<I")


def _load_all() -> dict:
    try:
        data = json.loads(KEYS_FILE.read_text())
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def load_key(device_id: str) -> bytes | None:
    try:
        key = bytes.fromhex(_load_all().get(device_id, ""))
    except (ValueError, TypeError):
        return None
    return key if len(key) == 32 else None


def save_key(device_id: str, key_hex: str) -> bool:
    """USB 로 받은 키를 저장한다. 새로 받았거나 바뀌었으면 True."""
    try:
        if len(bytes.fromhex(key_hex)) != 32 or not device_id:
            return False
    except (ValueError, TypeError):
        return False
    keys = _load_all()
    if keys.get(device_id) == key_hex:
        return False
    keys[device_id] = key_hex
    KEYS_FILE.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    tmp = KEYS_FILE.with_suffix(".tmp")
    # 처음부터 이 사용자만 읽고 쓸 수 있게 만든다 (Windows 는 사용자 폴더 권한을 따른다)
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(keys, f)
    os.replace(tmp, KEYS_FILE)
    try:
        os.chmod(KEYS_FILE, 0o600)
    except OSError:
        pass
    return True


def _mac(key: bytes, *parts: bytes) -> bytes:
    return hmac.new(key, b"".join(parts), hashlib.sha256).digest()


def http_header(key: bytes, nonce: bytes, ctr: int, path: str, body: bytes) -> str:
    tag = _mac(key, b"http", nonce, _CTR.pack(ctr), path.encode(), b"\n", body)
    return f"{nonce.hex()}.{ctr}.{tag.hex()}"


def tcp_tag(key: bytes, challenge: bytes) -> bytes:
    return _mac(key, b"tcp", challenge)


def udp_open(key: bytes, nonce: bytes, packet: bytes) -> tuple[int, bytes] | None:
    """서명이 맞으면 (순번, 내용), 아니면 None."""
    if len(packet) < UDP_TRAILER:
        return None
    body, ctr_bytes, tag = packet[:-UDP_TRAILER], packet[-UDP_TRAILER:-16], packet[-16:]
    want = _mac(key, b"udp", nonce, ctr_bytes, body)[:16]
    if not hmac.compare_digest(tag, want):
        return None
    return _CTR.unpack(ctr_bytes)[0], body


BLE_TAG = 16


def ble_hello_ok(key: bytes, pc_nonce: bytes, dev_nonce: bytes, tag: bytes) -> bool:
    """기기가 도전값에 답한 서명이 맞는지 (진짜 기기인지)."""
    return hmac.compare_digest(tag, _mac(key, b"ble-dev", pc_nonce, dev_nonce)[:BLE_TAG])


def ble_line(key: bytes, dev_nonce: bytes, ctr: int, body: bytes) -> bytes:
    """PC -> 기기 한 줄: #<순번>.<서명>.<JSON>"""
    tag = _mac(key, b"ble", dev_nonce, _CTR.pack(ctr), body)[:BLE_TAG]
    return f"#{ctr}.{tag.hex()}.".encode() + body + b"\n"


def ble_event_open(key: bytes, pc_nonce: bytes, data: bytes) -> tuple[int, bytes] | None:
    """기기 알림 <JSON>|<순번>.<서명> 의 서명이 맞으면 (순번, JSON), 아니면 None."""
    body, sep, trailer = data.rpartition(b"|")
    ctr_text, dot, tag_hex = trailer.partition(b".")
    if not sep or not dot or not ctr_text.isdigit() or len(tag_hex) != BLE_TAG * 2:
        return None
    try:
        tag = bytes.fromhex(tag_hex.decode())
        ctr = int(ctr_text)
    except ValueError:
        return None
    if not 0 < ctr <= 0xFFFFFFFF:
        return None
    if not hmac.compare_digest(tag, _mac(key, b"ble-evt", pc_nonce, _CTR.pack(ctr), body)[:BLE_TAG]):
        return None
    return ctr, body


class Window:
    """같은 순번은 한 번만 받는다. 가장 큰 순번과 그 아래 32개를 기억한다 (조금 뒤바뀐 순서는 받는다). 펌웨어와 같다."""

    def __init__(self):
        self.last = 0
        self.seen = 1  # 순번 0 은 받지 않는다

    def accept(self, ctr: int) -> bool:
        if ctr > self.last:
            shift = ctr - self.last
            self.seen = 1 if shift >= 32 else ((self.seen << shift) | 1) & 0xFFFFFFFF
            self.last = ctr
            return True
        back = self.last - ctr
        if back >= 32 or (self.seen >> back) & 1:
            return False
        self.seen |= 1 << back
        return True
