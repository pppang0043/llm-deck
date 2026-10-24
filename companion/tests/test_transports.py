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
    src = path.read_text()
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
