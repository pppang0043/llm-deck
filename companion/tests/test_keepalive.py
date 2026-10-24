import threading

import pytest

from transports.keepalive import KeepaliveTransport


class FakeTransport:
    name = "fake"

    def __init__(self, fail=False):
        self.messages = []
        self.beat = threading.Event()
        self.fail = fail
        self.closed = False
        self.on_confirm = None

    def send(self, message):
        assert not self.closed
        self.messages.append(message)
        self.beat.set()
        if self.fail:
            raise OSError("offline")

    def close(self):
        self.closed = True

    def connected(self):
        return not self.closed


def test_heartbeat_without_collector_and_stops_on_close():
    raw = FakeTransport()
    transport = KeepaliveTransport(raw, interval=0.01)
    try:
        assert raw.beat.wait(1)
        assert raw.messages[0] == {"heartbeat": True}
        assert transport.connected()
        transport.on_confirm = "callback"
        assert raw.on_confirm == "callback"
    finally:
        transport.close()
    assert not transport._thread.is_alive()
    assert not transport.connected()
    with pytest.raises(OSError):
        transport.send({"camera": "shoot"})


def test_heartbeat_failure_marks_connection_unusable():
    raw = FakeTransport(fail=True)
    transport = KeepaliveTransport(raw, interval=0.01)
    try:
        transport._thread.join(timeout=1)
        assert not transport._thread.is_alive()
        assert not transport.connected()
        with pytest.raises(OSError, match="offline"):
            transport.send({"camera": "shoot"})
    finally:
        transport.close()


def test_heartbeat_cannot_interleave_fragmented_message():
    raw = FakeTransport()
    entered, release = threading.Event(), threading.Event()
    original = raw.send

    def send(message):
        if "camera" in message:
            entered.set()
            assert release.wait(1)
        original(message)

    raw.send = send
    transport = KeepaliveTransport(raw, interval=0.02)
    sender = threading.Thread(target=transport.send, args=({"camera": "shoot"},))
    try:
        sender.start()
        assert entered.wait(1)
        assert not raw.beat.wait(0.06)
        release.set()
        sender.join(timeout=1)
        assert raw.beat.wait(1)
        assert raw.messages[0] == {"camera": "shoot"}
    finally:
        release.set()
        sender.join(timeout=1)
        transport.close()
