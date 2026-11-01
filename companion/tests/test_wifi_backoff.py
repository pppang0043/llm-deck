"""서명을 거절당한 도우미가 기기를 계속 두드리지 않는지. 가짜 시계와 가짜 HTTP 로 확인한다."""
import io
import json
import urllib.error

import pytest

from transports import auth, wifi

DEVICE = "aabbccddeeff"
ADDRESS = "192.0.2.5"
KEY_A, KEY_B = "11" * 32, "22" * 32
NONCE = "ab" * 16


class Reply(io.BytesIO):
    headers = {}
    def __enter__(self): return self
    def __exit__(self, *a): pass


@pytest.fixture
def device(monkeypatch):
    """가짜 기기. answers 에 넣은 대로 /usage 에 답하고, 받은 요청을 센다."""
    class Device:
        now = 1000.0
        status = 0
        usage = 0
        answers = None  # None 이면 계속 401 bad

    d = Device()
    wifi._refused.clear()
    monkeypatch.setattr(wifi.time, "monotonic", lambda: d.now)
    monkeypatch.setattr(wifi.WifiTransport, "_candidates", lambda self: iter([ADDRESS]))

    def urlopen(req, timeout=None):
        url = req if isinstance(req, str) else req.full_url
        if url.endswith("/status"):
            d.status += 1
            return Reply(json.dumps({"mic": True, "auth": 1, "id": DEVICE, "nonce": NONCE}).encode())
        d.usage += 1
        answer = d.answers.pop(0) if d.answers else "bad"
        if answer == "ok":
            return Reply(b"{}")
        raise urllib.error.HTTPError(url, 401, "no", {}, io.BytesIO(json.dumps({"auth": answer}).encode()))

    monkeypatch.setattr(wifi.urllib.request, "urlopen", urlopen)
    # 실제 포트를 잡지 않고 인증까지만 확인한다
    monkeypatch.setattr(wifi.socket, "gethostbyname", lambda name: name)
    monkeypatch.setattr(wifi, "LAST_ADDRESS", wifi.Path.home() / "device-address")

    class NoSocket:
        def __init__(self, *a): pass
        def setsockopt(self, *a): pass
        def bind(self, *a): pass
        def listen(self, *a): pass
        def close(self): pass
        def accept(self): raise OSError
        def recvfrom(self, *a): raise OSError

    monkeypatch.setattr(wifi.socket, "socket", NoSocket)
    auth.save_key(DEVICE, KEY_A)
    yield d
    wifi._refused.clear()


def attempt():
    """main.py 의 한 바퀴: 새 transport 를 열고 보내고, 실패하면 닫는다. 열렸으면 True."""
    t = wifi.WifiTransport({}, lambda *a: None)
    if not t.open():
        return False
    try:
        t.send({"services": []})
    except OSError:
        pass
    t.close()
    return True


def test_refused_helper_backs_off_exponentially_up_to_the_cap(device):
    assert attempt()
    waits = []
    for _ in range(9):
        started = device.now
        while not attempt():
            device.now += 1
        waits.append(device.now - started)
    assert waits[:6] == [60, 120, 240, 480, 960, 1800]
    assert waits[6:] == [1800] * 3
    assert device.usage == 10


def test_waiting_reaches_neither_usage_nor_status(device):
    assert attempt()
    status, usage = device.status, device.usage
    for _ in range(50):
        device.now += 1
        assert not attempt()
    assert (device.status, device.usage) == (status, usage)


def test_notice_once_per_streak_and_names_device_and_action(device, capsys):
    assert attempt()
    out = capsys.readouterr().out
    assert DEVICE in out and ADDRESS in out and "bad" in out and auth.NEED_USB in out
    for _ in range(4):
        device.now += 2000
        assert attempt()
    assert "거절" not in capsys.readouterr().out


def test_unpaired_also_backs_off(device):
    device.answers = ["unpaired"]
    assert attempt()
    assert not attempt()


def test_success_resets_the_streak(device, capsys):
    device.answers = ["bad", "bad", "ok"]
    assert attempt()
    device.now += 60
    assert attempt()
    device.now += 120
    assert attempt()  # 성공
    capsys.readouterr()
    assert attempt()  # 기다리지 않고 바로 시도, 다시 거절되면 1분부터
    assert "거절" in capsys.readouterr().out
    assert not attempt()
    device.now += 60
    assert attempt()


def test_new_key_from_usb_ends_the_wait_at_once(device):
    assert attempt()
    assert not attempt()
    auth.save_key(DEVICE, KEY_B)
    device.answers = ["ok"]
    assert attempt()
    assert not wifi._refused


def test_same_device_refusing_the_new_key_starts_a_new_streak(device, capsys):
    assert attempt()
    capsys.readouterr()
    auth.save_key(DEVICE, KEY_B)
    assert attempt()
    assert "거절" in capsys.readouterr().out
    device.now += 60
    assert attempt()


def test_stale_retries_once_at_once_and_does_not_back_off(device, capsys):
    device.answers = ["stale", "ok"]
    assert attempt()
    assert device.usage == 2 and device.status == 2
    assert not wifi._refused
    device.answers = ["stale", "stale"]
    assert attempt()
    assert not wifi._refused
    assert attempt()  # 기다리지 않는다
    assert "거절" in capsys.readouterr().out  # 위에서 남은 답이 없어 bad 로 끝난 이번 시도


def test_other_address_is_not_blocked(device, monkeypatch):
    assert attempt()
    monkeypatch.setattr(wifi.WifiTransport, "_candidates", lambda self: iter(["192.0.2.99"]))
    device.answers = ["ok"]
    t = wifi.WifiTransport({}, lambda *a: None)
    assert t.open()
    t.close()


def test_connect_falls_back_to_bluetooth_while_waiting(device):
    """main.connect 는 open() 이 False 인 Wi-Fi 를 건너뛴다. 예전에는 Wi-Fi 가 열려 블루투스까지 가지 못했다."""
    assert attempt()
    order = []

    class Ble:
        def __init__(self, *a): pass
        def open(self):
            order.append("ble")
            return True

    wifi_transport = wifi.WifiTransport({}, lambda *a: None)
    for name, make in (("wifi", lambda: wifi_transport), ("ble", Ble)):
        transport = make()
        if transport.open():
            break
        order.append(f"{name} 닫힘")
        transport.close()
    assert order == ["wifi 닫힘", "ble"]


def test_refusal_error_is_still_raised_for_the_main_loop(device):
    t = wifi.WifiTransport({}, lambda *a: None)
    assert t.open()
    with pytest.raises(OSError, match="인증 실패"):
        t.send({"services": []})
    t.close()
