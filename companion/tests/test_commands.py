import hashlib
import json
from pathlib import Path

import pytest

from transports.commands import CommandError, CommandStore, CommandTransport, CommandUncertain, COMMAND_FIELDS


class Device:
    name = "fake"
    device = "aabbccddeeff"

    def __init__(self, mode="done"):
        self.mode = mode
        self.on_event = lambda event: None
        self.messages = []
        self.results = {}
        self.executions = 0
        self.closed = False

    def send(self, message):
        self.messages.append(message)
        if "command_protocol" in message:
            if self.mode != "old":
                self.on_event({"command_proto": 1, "id": self.device})
        elif "command" in message:
            command = message["command"]
            key = (command["client"], command["seq"])
            if key not in self.results:
                self.executions += 1
                self.results[key] = "uncertain" if self.mode == "power_loss" else "done"
            if self.mode != "lost_ack":
                self.on_event({"ack": [*key, self.results[key]]})
        elif "command_status" in message:
            command = message["command_status"]
            key = (command["client"], command["seq"])
            self.on_event({"ack": [*key, self.results.get(key, "unknown")]})

    def connected(self):
        return not self.closed

    def close(self):
        self.closed = True


def transport(raw, path):
    return CommandTransport(raw, CommandStore(path), ack_timeout=0.01, command_timeout=0.2, poll_interval=0.001)


def test_lost_ack_is_queried_without_second_execution(tmp_path):
    raw = Device("lost_ack")
    t = transport(raw, tmp_path / "commands.json")
    try:
        t.send({"camera": "record", "services": [], "host": "test"})
        assert raw.executions == 1
        assert any("command_status" in message for message in raw.messages)
        assert any(message == {"services": [], "host": "test"} for message in raw.messages)
        assert t._store.pending(raw.device) is None
    finally:
        t.close()


def test_delayed_protocol_probe_retries_only_read_only_request(tmp_path):
    class BusyDevice(Device):
        def __init__(self):
            super().__init__()
            self.probes = 0

        def send(self, message):
            if "command_protocol" in message:
                self.probes += 1
                if self.probes < 3:
                    return  # 전송 부하로 처음 두 응답을 잃음
            super().send(message)

    raw = BusyDevice()
    t = transport(raw, tmp_path / "commands.json")
    try:
        t.send({"camera": "record"})
        assert raw.probes == 3
        assert raw.executions == 1
        assert len([m for m in raw.messages if "command" in m]) == 1
    finally:
        t.close()


def test_pc_and_device_restart_never_replay_uncertain_command(tmp_path):
    path = tmp_path / "commands.json"
    raw = Device("power_loss")
    t = transport(raw, path)
    try:
        with pytest.raises(CommandUncertain):
            t.send({"camera": "record"})
    finally:
        t.close()
    # 도우미를 다시 켜도 같은 client/순번의 미확정 기록이 남아 있다.
    restarted = Device()
    restarted.results = raw.results.copy()
    t = transport(restarted, path)
    try:
        with pytest.raises(CommandUncertain):
            t.send({"camera": "record"})
        assert restarted.executions == 0
        assert t.command_status()["state"] == "uncertain"
        assert t.command_status(acknowledge=True)["acknowledged"]
        assert restarted.executions == 0  # 기록 해제 자체는 실행이 아니다
        t.send({"camera": "close"})
        assert restarted.executions == 1
        assert json.loads(path.read_text())["next"] == 3
    finally:
        t.close()


def test_old_firmware_does_not_receive_unprotected_command(tmp_path):
    raw = Device("old")
    t = transport(raw, tmp_path / "commands.json")
    try:
        with pytest.raises(CommandError, match="proto 5"):
            t.send({"camera": "shoot"})
        assert not any("camera" in msg or "command" in msg for msg in raw.messages)
    finally:
        t.close()


def test_pc_record_failure_prevents_send(tmp_path, monkeypatch):
    raw = Device()
    t = transport(raw, tmp_path / "commands.json")
    monkeypatch.setattr(t._store, "_save", lambda: (_ for _ in ()).throw(OSError("disk full")))
    try:
        with pytest.raises(CommandError, match="저장 실패"):
            t.send({"camera": "shoot"})
        assert raw.executions == 0
    finally:
        t.close()


@pytest.mark.parametrize("after_write", [False, True])
def test_completion_save_failure_keeps_pending_and_prevents_duplicate(tmp_path, monkeypatch, after_write):
    raw = Device()
    t = transport(raw, tmp_path / "commands.json")
    save = t._store._save
    failed = False

    def fail_once():
        nonlocal failed
        if raw.executions == 1 and not failed:
            failed = True
            if after_write:
                save()  # rename 후 디렉터리 fsync 실패처럼 파일만 갱신된 경우
            raise OSError("disk full on completion")
        save()

    monkeypatch.setattr(t._store, "_save", fail_once)
    try:
        with pytest.raises(CommandUncertain):
            t.send({"camera": "record"})
        assert t._store.pending(raw.device) is not None
        t.send({"camera": "record"})
        assert raw.executions == 1
    finally:
        t.close()


def test_store_is_durable_and_does_not_contain_wifi_password(tmp_path):
    path = tmp_path / "commands.json"
    raw = Device("power_loss")
    t = transport(raw, path)
    try:
        with pytest.raises(CommandUncertain):
            t.send({"wifi_add": {"ssid": "my-wifi", "password": "do-not-persist"}})
        text = path.read_text()
        assert "do-not-persist" not in text and "my-wifi" not in text
        loaded = CommandStore(path)
        assert loaded.pending(raw.device)["seq"] == 1
        assert loaded.data["client"] == t._store.data["client"]
    finally:
        t.close()


@pytest.mark.parametrize("contents", ["bad json", "{}", '{"client":"x","next":1,"pending":{}}'])
def test_corrupt_pc_record_is_never_reset(tmp_path, contents):
    path = tmp_path / "commands.json"
    path.write_text(contents)
    with pytest.raises(CommandError):
        CommandStore(path)
    assert path.read_text() == contents


def test_pending_commands_are_separate_per_device(tmp_path):
    store = CommandStore(tmp_path / "commands.json")
    digest = hashlib.sha256(b"command").hexdigest()
    client, seq = store.begin("aabbccddeeff", digest)
    other_client, other_seq = store.begin("112233445566", digest)
    assert other_client == client and other_seq > seq
    store.finish("112233445566")
    assert store.pending("aabbccddeeff")["seq"] == seq


def test_firmware_and_companion_protect_same_commands():
    import re
    path = Path(__file__).resolve().parents[2] / "firmware/src/command_protocol.h"
    if not path.exists():
        pytest.skip("펌웨어 소스가 없는 배포본")
    block = path.read_text().split("COMMAND_FIELDS[] = {", 1)[1].split("};", 1)[0]
    assert set(re.findall(r'"([a-z0-9_]+)"', block)) == COMMAND_FIELDS


def test_pc_restart_after_device_completed_recovers_without_reexecution(tmp_path):
    path = tmp_path / "commands.json"
    store = CommandStore(path)
    payload = '{"camera":"record"}'
    client, seq = store.begin(Device.device, hashlib.sha256(payload.encode()).hexdigest())
    raw = Device()
    raw.results[(client, seq)] = "done"  # 기기만 완료 기록을 남긴 직후 PC 가 꺼짐
    t = transport(raw, path)
    try:
        t.send({"camera": "record"})
        assert raw.executions == 0
        assert t._store.pending(raw.device) is None
        assert t._store.data["next"] == 2
    finally:
        t.close()


def test_http_ack_signature_binds_nonce_counter_and_body():
    import hmac
    import struct
    from transports import auth
    key, nonce, body = bytes(32), bytes(16), b'{"ack":["client",1,"done"]}'
    tag = hmac.new(key, b"http-reply" + nonce + struct.pack("<I", 5) + body, hashlib.sha256).hexdigest()
    assert auth.http_reply_ok(key, nonce, 5, body, tag)
    assert not auth.http_reply_ok(key, nonce, 6, body, tag)
    assert not auth.http_reply_ok(key, b"x" * 16, 5, body, tag)
    assert not auth.http_reply_ok(key, nonce, 5, body + b" ", tag)
    assert not auth.http_reply_ok(key, nonce, 5, body, "")


@pytest.mark.parametrize("valid", [True, False])
def test_wifi_checks_signed_http_reply(monkeypatch, valid):
    import hmac
    import struct
    from transports import wifi
    w = wifi.WifiTransport({}, lambda *a: None)
    w.key, w.nonce, w.address = bytes(32), bytes(16), "127.0.0.1"
    body = b'{"command_proto":1,"id":"aabbccddeeff"}'
    tag = hmac.new(w.key, b"http-reply" + w.nonce + struct.pack("<I", 1) + body, hashlib.sha256).hexdigest()

    class Response:
        headers = {"X-Deck-Reply": tag if valid else "00" * 32}
        def __enter__(self): return self
        def __exit__(self, *args): pass
        def read(self, *args): return body

    monkeypatch.setattr(wifi.urllib.request, "urlopen", lambda *a, **kw: Response())
    if valid:
        assert w._request("/usage", b"{}", signed_reply=True) == body
    else:
        with pytest.raises(OSError, match="서명"):
            w._request("/usage", b"{}", signed_reply=True)
