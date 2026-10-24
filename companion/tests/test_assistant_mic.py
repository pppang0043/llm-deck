"""PC 마이크 고르기 (assistant.py). 진짜 마이크 대신 가짜 sounddevice 로 확인한다.

2026-10-24: 기본 마이크가 AirPods 일 때 소리가 오지 않아 받아쓰기가 계속 실패했고,
마이크를 연 뒤 PortAudio 를 다시 켜면 (sd._terminate) 도우미가 끝날 때까지 모든 마이크가 조용해지는 것을 Mac 에서 확인했다.
"""

import sys
import threading
import time

import numpy as np
import pytest

import assistant


class FakeStream:
    def __init__(self, sd, index):
        self.sd, self.device = sd, index
        self.loud = sd.devices[index]["name"] in sd.loud

    def start(self):
        self.started = time.monotonic()

    @property
    def read_available(self):
        return assistant.SAMPLE_RATE if self.loud else 0

    def read(self, n):
        return np.full((n, 1), 100, dtype=np.int16), False

    def abort(self):
        pass

    def close(self):
        pass


class FakeSd:
    def __init__(self, names, default, loud):
        self.devices = [{"name": n, "max_input_channels": 1, "hostapi": 0} for n in names]
        self.default, self.loud = default, set(loud)
        self.reinit = 0

    def query_devices(self, device=None, kind=None):
        if device is None and kind is None:
            return self.devices
        if device is None:
            return self.devices[self.default]
        return self.devices[device]

    def InputStream(self, samplerate, channels, dtype, device):
        return FakeStream(self, device)

    def _terminate(self):
        self.reinit += 1

    def _initialize(self):
        pass


@pytest.fixture
def fake(monkeypatch):
    sd = FakeSd(["AirPods Pro", "MacBook Pro 마이크", "BlackHole 2ch"], default=0, loud=["MacBook Pro 마이크"])
    monkeypatch.setitem(sys.modules, "sounddevice", sd)
    monkeypatch.setattr(sys, "platform", "darwin")
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "AirPods Pro")
    monkeypatch.setattr(assistant, "_lid_closed", lambda: False)
    monkeypatch.delenv(assistant._RESTARTED_ENV, raising=False)
    a = assistant.Assistant({})
    a.restarts = []
    a.restart = lambda: a.restarts.append(a.restart_for)
    return sd, a


def record(a, seconds=2.6):
    take = assistant._Take()
    a.take = take
    t = threading.Thread(target=a._record, args=(take,))
    t.start()
    time.sleep(seconds)
    a._end_take(take)
    t.join(5)
    return take


def test_order_starts_with_system_default_and_skips_virtual(fake):
    sd, a = fake
    assert a._mic_order(sd) == ["AirPods Pro", "MacBook Pro 마이크"]


def test_silent_default_moves_to_next_mic(fake):
    sd, a = fake
    take = record(a, 3.0)
    assert take.chunks, "AirPods 가 조용하면 MacBook 마이크로 녹음해야 한다"
    assert a._stream_name == "MacBook Pro 마이크"
    assert "AirPods" in a.pending_toast


def test_next_recording_starts_from_system_default_again(fake, monkeypatch):
    sd, a = fake
    record(a, 3.0)
    opened = []
    real = a._open_stream
    monkeypatch.setattr(a, "_open_stream", lambda sd_, name: opened.append(name) or real(sd_, name))
    record(a, 3.0)
    assert opened[0] == "AirPods Pro"


def test_never_reinit_portaudio_after_a_mic_was_opened(fake, monkeypatch):
    sd, a = fake
    record(a, 3.0)
    # 도우미를 켠 뒤 연결된 기본 마이크 (PortAudio 목록에 없다)
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "New AirPods")
    take = record(a, 3.0)
    assert sd.reinit == 0
    assert take.chunks
    assert a.restart_for == "기본 마이크 'New AirPods'"


def test_reinit_in_place_before_any_mic_was_opened(fake, monkeypatch):
    sd, a = fake
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "New AirPods")
    a._mic_order(sd)
    assert sd.reinit == 1
    assert a.restart_for is None


def test_restart_waits_until_idle_and_result_sent(fake):
    sd, a = fake
    a._opened = True
    a._refresh_devices(sd, "마이크 없음")
    a.take = assistant._Take()
    a.pending()
    a._end_take()
    a.busy = True
    a.pending()
    a.busy = False
    a.pending_toast = "입력했습니다"
    a.pending()
    assert a.restarts == []
    a.pending_toast = None
    a.pending()
    assert a.restarts == ["마이크 없음"]


def test_same_reason_does_not_restart_again_soon(fake, monkeypatch):
    sd, a = fake
    monkeypatch.setenv(assistant._RESTARTED_ENV, f"{time.time():.0f}|마이크 없음")
    a._opened = True
    a._refresh_devices(sd, "마이크 없음")
    assert a.restart_for is None
