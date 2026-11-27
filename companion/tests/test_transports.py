"""자동 선택(auto) 순서. 기기의 우선순위(firmware/src/link.cpp)와 같아야 한다."""

import re
import types
from pathlib import Path

import pytest

from transports import TRANSPORTS


def test_auto_order_usb_wifi_ble():
    assert list(TRANSPORTS) == ["usb", "wifi", "ble"]


def test_firmware_priority_matches():
    path = Path(__file__).resolve().parents[2] / "firmware" / "src" / "link.cpp"
    if not path.exists():
        pytest.skip("펌웨어 소스가 없는 배포본")
    src = path.read_text(encoding="utf-8")
    rank = {name: int(n) for name, n in re.findall(r"case LINK_(USB|WIFI|BLE): return (\d);", src)}
    assert rank["USB"] > rank["WIFI"] > rank["BLE"]


def test_wifi_skipped_when_off_on_device():
    from transports.wifi import WifiTransport

    w = WifiTransport({}, lambda *a: None)
    assert w._use_status({"link": False, "auth": 1, "id": "x", "nonce": "00" * 16}) is False


def test_wifi_send_fails_when_device_turned_it_off():
    from transports.wifi import WifiTransport

    w = WifiTransport({}, lambda *a: None)
    w._request = lambda path, body=None, timeout=5: b'{"ok":true,"mic":false,"link":false}'
    with pytest.raises(OSError):
        w.send({"host": "t"})
    w._request = lambda path, body=None, timeout=5: b'{"ok":true,"mic":false,"link":true}'
    w.send({"host": "t"})
    w._request = lambda path, body=None, timeout=5: b'{"ok":true,"mic":false}'  # 옛 펌웨어
    w.send({"host": "t"})


def test_wifi_touch_reports_missing_control_ownership():
    from transports.wifi import WifiTransport

    w = WifiTransport({}, lambda *a: None)
    w._request = lambda *a, **kw: b'{"link_error":"not_owner"}'
    with pytest.raises(OSError, match="다른 PC"):
        w.send({"touch": {"x": 1, "y": 1, "down": True}})


def test_usb_announces_only_after_deck_confirmed(tmp_path, monkeypatch):
    # 다른 ESP32 보드에 미러링 창을 열지 않도록, LLM Deck 로그가 보인 뒤에 한 번만 알린다
    from transports import usb

    monkeypatch.setattr(usb, "KNOWN_FILE", tmp_path / "usb_serial")
    t = usb.UsbTransport({}, lambda seq, pcm: None)
    t.serial_number = "AA"
    calls = []
    t.on_confirm = lambda: calls.append(1)
    assert calls == []
    t._confirm()
    t._confirm()
    t.take_confirm()
    assert calls == [1]
    assert (tmp_path / "usb_serial").read_text() == "AA"


def test_ble_close_frees_event_loop():
    # 연결 시도마다 새로 만들므로 닫으면 루프와 스레드가 끝나야 한다 (파일 핸들이 쌓이지 않게)
    from transports.ble import BleTransport

    t = BleTransport({}, lambda *a: None)
    t.close()
    assert t.loop.is_closed()
    assert not t._thread.is_alive()


def test_usb_stays_off_the_port_while_flashing(tmp_path, monkeypatch):
    # 올리기 도구와 같이 포트를 읽으면 올리기가 중간에 끊겨 기기가 반쯤 써진 채 멈춘다
    from transports import usb

    flag = tmp_path / "flashing"
    monkeypatch.setattr(usb, "FLASHING_FILE", flag)
    assert not usb.flashing()
    flag.touch()
    assert usb.flashing()
    opened = []
    monkeypatch.setattr(usb, "find_port", lambda fixed="": opened.append(1) or ("/dev/x", "AA"))
    assert not usb.UsbTransport({}, lambda seq, pcm: None).open()
    assert opened == []
    old = usb.time.time() - usb.FLASHING_MAX_SECONDS - 1
    import os
    os.utime(flag, (old, old))  # 올리기가 죽어 남은 표시
    assert not usb.flashing()


def test_wifi_retries_only_explicit_queue_rejection(monkeypatch):
    import io
    import urllib.error
    from transports import wifi

    w = wifi.WifiTransport({}, lambda *a: None)
    calls = []
    monkeypatch.setattr(wifi.time, "sleep", lambda seconds: None)

    def request(path, body):
        calls.append(body)
        if len(calls) < 3:
            raise urllib.error.HTTPError("http://device/usage", 503, "busy", {},
                                         io.BytesIO(b'{"error":"not_queued"}'))
        return b'{"ok":true}'

    w._request = request
    w.send({"camera": "record"})
    assert len(calls) == 3 and calls[0] == calls[1] == calls[2]


@pytest.mark.parametrize("failure", ["timeout", "unknown503", "full"])
def test_wifi_never_blindly_replays_camera_toggle(monkeypatch, failure):
    import io
    import urllib.error
    from transports import wifi

    w = wifi.WifiTransport({}, lambda *a: None)
    calls = []
    monkeypatch.setattr(wifi.time, "sleep", lambda seconds: None)

    def request(path, body):
        calls.append(body)
        if failure == "timeout":
            raise TimeoutError("response lost")
        response = b'{"error":"not_queued"}' if failure == "full" else b'busy'
        raise urllib.error.HTTPError("http://device/usage", 503, "busy", {}, io.BytesIO(response))

    w._request = request
    with pytest.raises(OSError):
        w.send({"camera": "record"})
    assert len(calls) == (3 if failure == "full" else 1)


def test_wifi_tcp_keeps_reading_when_signature_comes_alone():
    # 2026-10-26: 기기가 서명만 먼저 보내면 도우미가 바로 끊어 Wi-Fi 미러링 화면이 오지 않았다
    import socket
    import threading
    import time

    from transports import auth
    from transports.wifi import WifiTransport

    events = []
    w = WifiTransport({}, lambda *a: None, on_event=events.append)
    w.key = b"k" * 32
    pc, device = socket.socketpair()
    reader = threading.Thread(target=w._read_stream, args=(pc,), daemon=True)
    reader.start()
    challenge = device.recv(16)
    device.sendall(b"\xa5\x5a" + bytes([auth.FRAME_AUTH, 32, 0]) + auth.tcp_tag(w.key, challenge))
    time.sleep(0.2)  # 서명만 따로 닿게
    body = b'{"hi": 1}'
    device.sendall(b"\xa5\x5a" + bytes([2, len(body), 0]) + body)
    deadline = time.monotonic() + 2
    while not events and time.monotonic() < deadline:
        time.sleep(0.02)
    device.close()
    reader.join(2)
    assert events == [{"hi": 1}]


def _board(serial_number):
    from transports import usb
    return types.SimpleNamespace(vid=usb.ESPRESSIF_VID, pid=usb.USB_JTAG_PID, serial_number=serial_number, device=f"/dev/{serial_number}")


def test_usb_opens_only_the_registered_board(tmp_path, monkeypatch):
    from transports import usb

    known = tmp_path / "usb_serial"
    known.write_text("DECK")
    monkeypatch.setattr(usb, "KNOWN_FILE", known)
    monkeypatch.setattr(usb.list_ports, "comports", lambda: [_board("STICK"), _board("DECK")])
    assert usb.find_port() == ("/dev/DECK", "DECK")
    monkeypatch.setattr(usb.list_ports, "comports", lambda: [_board("STICK")])
    assert usb.find_port() is None


def test_usb_tries_any_board_before_one_is_registered(tmp_path, monkeypatch):
    from transports import usb

    monkeypatch.setattr(usb, "KNOWN_FILE", tmp_path / "usb_serial")
    monkeypatch.setattr(usb.list_ports, "comports", lambda: [_board("NEW")])
    assert usb.find_port() == ("/dev/NEW", "NEW")


def test_usb_read_loop_does_not_wait_for_a_full_block():
    """read(4096) 은 4096 바이트가 찰 때까지 timeout 을 기다려 미러링 장 끝이 0.5초 늦었다. 와 있는 만큼만 읽는다"""
    import struct

    from transports import usb

    frame = b"\xa5\x5a\x02" + struct.pack("<H", 2) + b"{}"

    class Ser:
        is_open = True
        pending = frame

        @property
        def in_waiting(self):
            return len(self.pending)

        def read(self, size):
            if not self.pending:
                self.is_open = False
                return b""
            assert size <= len(self.pending), "와 있는 것보다 많이 달라고 하면 timeout 까지 기다린다"
            data, self.pending = self.pending[:size], self.pending[size:]
            return data

    events = []
    t = usb.UsbTransport({}, lambda seq, pcm: None, on_event=events.append)
    t._read_loop(Ser())
    assert events == [{}]


def test_usb_write_timeout_logs_both_directions(capsys):
    """회사 PC 의 USB 멈춤: 쓰기가 끊기면 마지막으로 받은 때·쓴 때·오간 양을 한 줄로 남긴다."""
    import pytest
    import serial

    from transports import usb

    class Ser:
        port, is_open, in_waiting, out_waiting = "COM3", True, 0, 4096

        def write(self, data):
            raise serial.SerialTimeoutException("Write timeout")

    t = usb.UsbTransport({}, lambda seq, pcm: None)
    t.ser, t.confirmed, t.paired = Ser(), True, True
    t.opened_at = t.last_rx = 1.0
    t.rx_bytes = 1234
    with pytest.raises(OSError):
        t.send({"x": 1})
    line = capsys.readouterr().out
    assert "[USB-diag] 쓰기 실패 (SerialTimeoutException: Write timeout)" in line
    assert "COM3" in line and "rx 1234 B" in line and "last write never" in line and "out 4096 B" in line


@pytest.mark.parametrize("platform, lines", [("win32", (False, False)), ("darwin", (True, True))])
def test_usb_open_keeps_reset_lines_quiet_on_windows(monkeypatch, tmp_path, platform, lines):
    # ESP32-S3 는 DTR 꺼짐 + RTS 켜짐이 되면 재시작한다. Windows 는 열기 전에 둘 다 끄고, Mac 은 기본값(켜짐) 그대로 연다
    from transports import usb

    seen = []

    class Ser:
        def __init__(self, port, *a, **k):
            assert port is None  # 신호를 정하기 전에 열리면 안 된다
            self.port, self.dtr, self.rts, self.is_open = None, True, True, False

        def open(self):
            seen.append((self.port, self.dtr, self.rts))
            self.is_open = True

        def read(self, n):
            self.is_open = False
            return b""

    monkeypatch.setattr(usb.sys, "platform", platform)
    monkeypatch.setattr(usb.serial, "Serial", Ser)
    monkeypatch.setattr(usb, "find_port", lambda fixed="": ("COM3", "AA"))
    monkeypatch.setattr(usb, "FLASHING_FILE", tmp_path / "none")
    monkeypatch.setattr(usb, "KNOWN_FILE", tmp_path / "usb_serial")
    assert usb.UsbTransport({}, lambda seq, pcm: None).open()
    assert seen == [("COM3", *lines)]


def test_usb_note_tells_device_why_it_reopened(monkeypatch):
    # 다시 연 이유는 처음 고른 것만 남고, 기기가 무언가 보낸 뒤 한 번만 {"usb_note"} 로 간다 (기기 [usb-log] 27자)
    from transports import usb

    monkeypatch.setattr(usb, "_close_why", "closed")
    monkeypatch.setattr(usb, "_opens", 12)
    usb._closing("write-fail")
    usb._closing("read-end")
    note = usb._note()
    assert note.startswith("pc write-fail x12 ") and len(note) <= 27

    written = []

    class Ser:
        port, is_open = "COM3", True

        def write(self, data):
            written.append(data)
            return len(data)

    t = usb.UsbTransport({}, lambda seq, pcm: None)
    t.ser, t.confirmed, t.paired, t.note = Ser(), True, True, note
    t.opened_at = t.last_rx = 5.0
    t.send({"x": 1})
    assert len(written) == 1  # 기기에서 아직 아무것도 못 받았다
    t.last_rx = 6.0
    t.send({"x": 1})
    t.send({"x": 1})
    notes = [w for w in written if b"usb_note" in w]
    assert notes == [('{"usb_note": "%s"}\n' % note).encode()]
