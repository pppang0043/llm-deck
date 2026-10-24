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
    sd = FakeSd(["USB 마이크", "MacBook Pro 마이크", "BlackHole 2ch"], default=0, loud=["MacBook Pro 마이크"])
    monkeypatch.setitem(sys.modules, "sounddevice", sd)
    monkeypatch.setattr(sys, "platform", "darwin")
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "USB 마이크")
    monkeypatch.setattr(assistant, "_lid_closed", lambda: False)
    monkeypatch.setattr(assistant, "_mac_use_builtin_input", lambda: None)  # 시험이 이 Mac 의 기본 입력을 바꾸지 않게
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
    assert a._mic_order(sd) == ["USB 마이크", "MacBook Pro 마이크"]


def test_silent_default_moves_to_next_mic(fake):
    sd, a = fake
    take = record(a, 3.0)
    assert take.chunks, "USB 마이크가 조용하면 MacBook 마이크로 녹음해야 한다"
    assert a._stream_name == "MacBook Pro 마이크"
    assert "USB" in a.pending_toast


def test_quiet_mic_goes_last_for_a_while(fake, monkeypatch):
    sd, a = fake
    record(a, 3.0)
    assert a.restart_for == "'USB 마이크' 마이크가 조용함"
    opened = []
    real = a._open_stream
    monkeypatch.setattr(a, "_open_stream", lambda sd_, name: opened.append(name) or real(sd_, name))
    a._close_stream()
    a.pending_toast = None
    take = record(a, 2.0)
    # 녹음마다 USB 마이크부터 1.5초씩 버리고 "바꿉니다" 알림을 띄우지 않는다
    assert opened == ["MacBook Pro 마이크"]
    assert take.chunks
    assert not a.pending_toast


def test_quiet_mic_tried_first_again_later(fake, monkeypatch):
    sd, a = fake
    record(a, 3.0)
    monkeypatch.setattr(assistant.time, "monotonic", lambda: time.perf_counter() + assistant.QUIET_MIC_SECONDS + 1)
    assert a._mic_order(sd)[0] == "USB 마이크"


def test_bluetooth_default_mic_is_never_used(fake, monkeypatch):
    sd, a = fake
    sd.devices[0]["name"] = "AirPods Pro"
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "AirPods Pro")
    # 기본 마이크가 AirPods 여도 통화용(HFP) 으로 바뀌지 않게 MacBook 마이크로 녹음한다
    assert a._mic_order(sd) == ["MacBook Pro 마이크"]
    take = record(a, 2.0)
    assert take.chunks
    assert a._stream_name == "MacBook Pro 마이크"
    assert not a.pending_toast


def test_bluetooth_mic_not_used_even_if_chosen_in_config(fake):
    sd, a = fake
    sd.devices[0]["name"] = "AirPods Pro"
    a.cfg["input"] = "AirPods"
    assert "AirPods Pro" not in a._mic_order(sd)


def test_new_airpods_default_does_not_reread_devices(fake, monkeypatch):
    sd, a = fake
    a._opened = True
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "New AirPods")
    assert a._mic_order(sd)[0] == "USB 마이크"
    assert a.restart_for is None


def test_airpods_default_input_switched_back_every_2_seconds(fake, monkeypatch):
    sd, a = fake
    calls = []
    monkeypatch.setattr(assistant, "_mac_use_builtin_input", lambda: calls.append(1) and None)
    a.pending()
    a.pending()  # 2초 안에는 다시 보지 않는다
    assert len(calls) == 1
    a._input_checked -= 2
    a.pending()
    assert len(calls) == 2


def test_never_reinit_portaudio_after_a_mic_was_opened(fake, monkeypatch):
    sd, a = fake
    record(a, 3.0)
    a.restart_for = None  # 조용했던 USB 마이크 때문에 잡힌 다시 띄우기는 이 확인과 상관없다
    # 도우미를 켠 뒤 연결된 기본 마이크 (PortAudio 목록에 없다)
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "New USB Mic")
    take = record(a, 3.0)
    assert sd.reinit == 0
    assert take.chunks
    assert a.restart_for == "기본 마이크 'New USB Mic'"


def test_reinit_in_place_before_any_mic_was_opened(fake, monkeypatch):
    sd, a = fake
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "New USB Mic")
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


def test_windows_mme_name_cut_at_31_chars_matches():
    full = "Headset Microphone (WH-1000XM4 Hands-Free AG Audio)"
    assert assistant._match_input(full, ["Microphone Array (Realtek(R) A", full[:31]]) == full[:31]
    assert assistant._match_input("Mic", ["Microphone"]) is None


def test_windows_order_uses_real_default_and_skips_aliases(monkeypatch):
    sd = FakeSd(["Microsoft Sound Mapper - Input", "마이크 배열(Realtek(R) Audio)", "헤드셋 마이크(USB Audio)",
                 "Primary Sound Capture Driver"], default=0, loud=[])
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "헤드셋 마이크(USB Audio)")
    a = assistant.Assistant({})
    assert a._mic_order(sd) == ["헤드셋 마이크(USB Audio)", "마이크 배열(Realtek(R) Audio)"]


def test_windows_order_falls_back_to_sound_mapper(monkeypatch):
    sd = FakeSd(["Microsoft Sound Mapper - Input", "마이크 배열(Realtek(R) Audio)"], default=0, loud=[])
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setattr(assistant, "_system_default_input", lambda: None)  # Core Audio 를 못 읽었다
    a = assistant.Assistant({})
    assert a._mic_order(sd) == ["Microsoft Sound Mapper - Input", "마이크 배열(Realtek(R) Audio)"]


def test_windows_unmatched_default_reread_only_once(monkeypatch):
    sd = FakeSd(["Microsoft Sound Mapper - Input", "마이크 배열(Realtek(R) Audio)"], default=0, loud=[])
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "이름이 다르게 나오는 마이크")
    a = assistant.Assistant({})
    a._opened = True  # Windows 는 마이크를 연 뒤에도 그 자리에서 다시 읽는다
    for _ in range(3):
        assert a._mic_order(sd)[0] == "Microsoft Sound Mapper - Input"
    assert sd.reinit == 1
