"""블루투스 서명 (auth.py 의 ble_*, 펌웨어 ble_link.h 와 같은 형식)."""

import hashlib
import hmac
import struct

from transports import auth

KEY = bytes(range(32))
PC = b"p" * 16
DEV = b"d" * 16


def _event(key, ctr, body):
    tag = hmac.new(key, b"ble-evt" + PC + struct.pack("<I", ctr) + body, hashlib.sha256).digest()[:16]
    return body + f"|{ctr}.{tag.hex()}".encode()


def test_hello_tag():
    tag = hmac.new(KEY, b"ble-dev" + PC + DEV, hashlib.sha256).digest()[:16]
    assert auth.ble_hello_ok(KEY, PC, DEV, tag)
    assert not auth.ble_hello_ok(bytes(32), PC, DEV, tag)


def test_line_format():
    line = auth.ble_line(KEY, DEV, 7, b'{"a":1}')
    head, tag_hex, body = line.rstrip(b"\n").split(b".", 2)
    assert head == b"#7" and body == b'{"a":1}' and line.endswith(b"\n")
    want = hmac.new(KEY, b"ble" + DEV + struct.pack("<I", 7) + body, hashlib.sha256).digest()[:16]
    assert bytes.fromhex(tag_hex.decode()) == want


def test_event_open():
    body = b'{"x":"a|b"}'  # JSON 안의 | 는 괜찮다 (마지막 | 로 나눈다)
    assert auth.ble_event_open(KEY, PC, _event(KEY, 3, body)) == (3, body)
    assert auth.ble_event_open(bytes(32), PC, _event(KEY, 3, body)) is None  # 다른 키 (가짜 기기)
    assert auth.ble_event_open(KEY, PC, body) is None  # 서명 없음
    forged = _event(KEY, 3, body).replace(b'"a', b'"z')
    assert auth.ble_event_open(KEY, PC, forged) is None
