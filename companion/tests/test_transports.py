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
