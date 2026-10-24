"""자동 선택(auto) 순서. 기기의 우선순위(firmware/src/link.cpp)와 같아야 한다."""

import re
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
