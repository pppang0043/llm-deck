"""PC 마이크 고르기 (assistant.py, micproc.py). 진짜 마이크 대신 가짜 sounddevice 를 녹음 프로세스 자리에 넣는다.

2026-10-24: 기본 마이크가 AirPods 일 때 소리가 오지 않아 받아쓰기가 계속 실패했고,
마이크를 연 뒤 PortAudio 를 다시 켜면 (sd._terminate) 그 프로세스가 끝날 때까지 모든 마이크가 조용해지는 것을 Mac 에서 확인했다.
그래서 마이크는 녹음 프로세스에서만 열고, 장치 목록을 새로 읽을 때는 그 프로세스만 다시 띄운다.
"""

import sys
import threading
import time

import numpy as np
import pytest

import assistant
import micproc


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

    def query_devices(self, device=None, kind=None):
        if device is None and kind is None:
            return self.devices
        if device is None:
            return self.devices[self.default]
        return self.devices[device]

    def InputStream(self, samplerate, channels, dtype, device):
        return FakeStream(self, device)


class LocalMic:
    """녹음 프로세스 대신 같은 프로세스에서 MicServer 를 돌린다. 띄울 때의 장치 목록을 들고 있다 (PortAudio 처럼)."""

    def __init__(self, sd, on_message):
        snapshot = FakeSd([d["name"] for d in sd.devices], sd.default, ())
        snapshot.loud = sd.loud  # 소리는 지금 것을 따른다
        self.server = micproc.MicServer(snapshot, on_message)
        self.up = True

    def alive(self):
        return self.up

    def send(self, cmd):
        self.server.handle(cmd)

    def close(self):
        self.server._halt()
        self.server._close()
        self.up = False


@pytest.fixture(autouse=True)
def mic_files(tmp_path, monkeypatch):
    # 이 Mac 의 설정을 건드리지 않게
    monkeypatch.setattr(assistant, "FALLBACK_MIC_FILE", tmp_path / "fallback_mic.txt")
    monkeypatch.setattr(assistant, "PICKED_MIC_FILE", tmp_path / "picked_mic.txt")
    monkeypatch.setattr(assistant, "PHONE_MICS_FILE", tmp_path / "phone_mics.txt")
    return tmp_path


@pytest.fixture
def fallback_file(mic_files):
    return mic_files / "fallback_mic.txt"


def make(monkeypatch, sd, platform="darwin", default="USB 마이크"):
    monkeypatch.setattr(sys, "platform", platform)
    monkeypatch.setattr(assistant, "_system_default_input", lambda: default)
    monkeypatch.setattr(assistant, "_mac_default_input", lambda: default)
    monkeypatch.setattr(assistant, "_mac_inputs", lambda: {d["name"]: (i, "usb ") for i, d in enumerate(sd.devices)})
    monkeypatch.setattr(assistant, "_lid_closed", lambda: False)
    monkeypatch.setattr(assistant, "_mac_use_builtin_input", lambda allowed="": None)  # 시험이 이 Mac 의 기본 입력을 바꾸지 않게
    a = assistant.Assistant({})
    a.mics = assistant.MicClient(lambda on: LocalMic(sd, on))
    return a


@pytest.fixture
def fake(monkeypatch):
    sd = FakeSd(["USB 마이크", "MacBook Pro 마이크", "BlackHole 2ch"], default=0, loud=["MacBook Pro 마이크"])
    return sd, make(monkeypatch, sd)


def record(a, seconds=2.6):
    take = assistant._Take()
    a.take = take
    t = threading.Thread(target=a._record, args=(take,))
    t.start()
    time.sleep(seconds)
    a._end_take(take)
    t.join(5)
    return take


def probe(a):
    """미리 들어 보기를 한 번 (스레드 없이)"""
    a._probing = True
    a._probe_round()


def test_order_starts_with_system_default_and_skips_virtual(fake):
    sd, a = fake
    assert a._mic_order() == ["USB 마이크", "MacBook Pro 마이크"]


def test_silent_default_moves_to_next_mic(fake):
    sd, a = fake
    take = record(a, 3.0)
    assert take.chunks, "USB 마이크가 조용하면 MacBook 마이크로 녹음해야 한다"
    assert take.mic == "MacBook Pro 마이크"
    assert "USB" in a.pending_toast
    assert (a.mic_state, a.mic_name) == ("ok", "MacBook Pro 마이크")


def test_quiet_mic_goes_last_for_a_while(fake, monkeypatch):
    sd, a = fake
    record(a, 3.0)
    opened = []
    real = a.mics.listen
    monkeypatch.setattr(a.mics, "listen", lambda name: opened.append(name) or real(name))
    a.pending_toast = None
    take = record(a, 2.0)
    # 녹음마다 USB 마이크부터 1.5초씩 버리고 "바꿉니다" 알림을 띄우지 않는다
    assert opened == ["MacBook Pro 마이크"]
    assert take.chunks
    assert not a.pending_toast


def test_fallback_mic_kept_after_quiet_time(fake, monkeypatch):
    sd, a = fake
    record(a, 3.0)
    monkeypatch.setattr(assistant.time, "monotonic", lambda: time.perf_counter() + assistant.QUIET_MIC_SECONDS + 1)
    # 10분이 지나도 대신 쓴 마이크를 먼저 쓴다 (다시 조용한 기본 마이크부터 써 보고 알림을 띄우지 않게)
    assert a._mic_order()[0] == "MacBook Pro 마이크"


def test_fallback_mic_survives_helper_restart(fake, fallback_file, monkeypatch):
    sd, a = fake
    record(a, 3.0)
    assert fallback_file.read_text(encoding="utf-8") == "MacBook Pro 마이크"
    again = make(monkeypatch, sd)
    assert again._mic_order()[0] == "MacBook Pro 마이크"


def test_fallback_mic_forgotten_when_unplugged(fake, fallback_file):
    sd, a = fake
    record(a, 3.0)
    a._quiet_until.clear()
    del sd.devices[1]
    a.mics.restart()  # 장치가 바뀌면 녹음 프로세스를 새로 띄운다 (_watch_mics)
    assert a._mic_order()[0] == "USB 마이크"
    assert not fallback_file.exists()


def test_fallback_mic_replaced_when_it_goes_quiet(fake, fallback_file):
    sd, a = fake
    record(a, 3.0)
    a._quiet_until.clear()
    sd.loud = {"USB 마이크"}
    a.mics.restart()
    take = record(a, 3.0)
    assert take.chunks and take.mic == "USB 마이크"
    # 조용해진 MacBook 마이크 대신 이번에 소리가 온 마이크를 기억한다
    assert fallback_file.read_text(encoding="utf-8") == "USB 마이크"


def test_bluetooth_default_mic_is_never_used(fake, monkeypatch):
    sd, a = fake
    sd.devices[0]["name"] = "AirPods Pro"
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "AirPods Pro")
    # 기본 마이크가 AirPods 여도 통화용(HFP) 으로 바뀌지 않게 MacBook 마이크로 녹음한다
    assert a._mic_order() == ["MacBook Pro 마이크"]
    take = record(a, 2.0)
    assert take.chunks and take.mic == "MacBook Pro 마이크"
    assert not a.pending_toast


def test_bluetooth_found_by_connection_not_name(fake, monkeypatch):
    sd, a = fake
    sd.devices[0]["name"] = "내 이어폰"
    monkeypatch.setattr(assistant, "_mac_inputs", lambda: {"내 이어폰": (1, "blue"), "MacBook Pro 마이크": (2, "bltn")})
    a._device_signature()
    assert "내 이어폰" not in a._mic_order()


def test_bluetooth_mic_not_used_even_if_chosen_in_config(fake):
    sd, a = fake
    sd.devices[0]["name"] = "AirPods Pro"
    a.cfg["input"] = "AirPods"
    assert "AirPods Pro" not in a._mic_order()


def test_new_airpods_default_does_not_respawn_recorder(fake, monkeypatch):
    sd, a = fake
    a._mic_order()
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "New AirPods")
    assert a._mic_order()[0] == "USB 마이크"
    assert a.mics.spawned == 1


def test_new_default_mic_respawns_only_the_recorder(fake, monkeypatch):
    sd, a = fake
    record(a, 3.0)
    # 녹음 프로세스를 띄운 뒤 연결된 기본 마이크 (그 프로세스의 PortAudio 목록에는 없다)
    sd.devices.append({"name": "New USB Mic", "max_input_channels": 1, "hostapi": 0})
    sd.loud.add("New USB Mic")
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "New USB Mic")
    a.on_event({"mic_pick": "New USB Mic"})  # 대신 쓰던 MacBook 마이크보다 먼저 쓰게
    take = record(a, 3.0)
    assert a.mics.spawned == 2  # 도우미는 그대로, 녹음 프로세스만 새로 띄웠다
    assert take.chunks and take.mic == "New USB Mic"


def test_unknown_default_name_respawns_only_once(fake, monkeypatch):
    sd, a = fake
    monkeypatch.setattr(assistant, "_system_default_input", lambda: "이름이 다르게 나오는 마이크")
    for _ in range(3):
        assert a._mic_order()[0] == "USB 마이크"
    assert a.mics.spawned == 2


def test_probe_finds_working_mic_before_recording(fake):
    sd, a = fake
    probe(a)
    assert (a.mic_state, a.mic_name) == ("ok", "MacBook Pro 마이크")
    assert a._check_due is None
    assert not a.pending_toast  # 미리 들어 볼 때는 알림을 띄우지 않는다
    # 녹음 버튼을 누르면 조용한 USB 마이크를 건너뛰고 바로 MacBook 마이크로
    assert a._mic_order()[0] == "MacBook Pro 마이크"


def test_probe_retries_then_reports_no_mic(fake):
    sd, a = fake
    sd.loud.clear()
    states = []
    for _ in range(assistant.PROBE_TRIES):
        a._quiet_until.clear()
        probe(a)
        states.append(a.mic_state)
    assert states == ["checking"] * (assistant.PROBE_TRIES - 1) + ["none"]
    assert not a.mic and "소리가 오지 않습니다" in a.mic_reason
    assert a._check_due > time.monotonic() + assistant.RECHECK_SECONDS - 5
    status = a.mic_status()
    assert status["state"] == "none" and status["why"]


def test_watch_probes_when_devices_change(fake, monkeypatch):
    sd, a = fake
    rounds = []
    monkeypatch.setattr(a, "_probe_round", lambda: rounds.append(1) or setattr(a, "_probing", False))
    a._watch_mics()
    time.sleep(0.05)
    a._check_due = None
    a._watched -= 2
    a._watch_mics()
    time.sleep(0.05)
    assert len(rounds) == 1  # 그대로면 다시 들어 보지 않는다
    sd.devices.append({"name": "New USB Mic", "max_input_channels": 1, "hostapi": 0})
    a._watched -= 2
    a._watch_mics()
    time.sleep(0.05)
    assert len(rounds) == 2 and not a._fresh  # 새 장치를 알도록 녹음 프로세스를 새로 띄운 뒤 들어 본다


def test_airpods_default_input_switched_back_every_2_seconds(fake, monkeypatch):
    sd, a = fake
    calls = []
    monkeypatch.setattr(assistant, "_mac_use_builtin_input", lambda allowed="": calls.append(allowed) and None)
    monkeypatch.setattr(a, "_probe_round", lambda: setattr(a, "_probing", False))
    a.pending()
    a.pending()  # 2초 안에는 다시 보지 않는다
    assert len(calls) == 1
    a._watched -= 2
    a.pending()
    assert len(calls) == 2


def test_picked_mic_goes_first_and_is_rechecked(fake, mic_files):
    sd, a = fake
    sd.loud.add("USB 마이크")
    a._check_due = None
    a.on_event({"mic_pick": "MacBook Pro 마이크"})
    assert a._mic_order()[0] == "MacBook Pro 마이크"
    assert a._check_due is not None
    assert (mic_files / "picked_mic.txt").read_text(encoding="utf-8") == "MacBook Pro 마이크"
    assert a.mic_status()["pick"] == "MacBook Pro 마이크"
    a.on_event({"mic_pick": ""})  # 자동
    assert a._mic_order()[0] == "USB 마이크"
    assert not (mic_files / "picked_mic.txt").exists()


def test_status_lists_choosable_mics(fake):
    sd, a = fake
    a._mic_order()
    assert a.mic_status()["list"] == ["USB 마이크", "MacBook Pro 마이크"]


def test_unanswered_recorder_is_replaced(fake):
    sd, a = fake

    class Stuck:
        def alive(self):
            return True

        def send(self, cmd):
            pass  # 답하지 않는다

        def close(self):
            pass

    a.mics._spawn = lambda on: Stuck()
    with pytest.raises(OSError):
        a.mics._ask({"op": "devices"}, 0.1)
    assert a.mics._proc is None  # 다음에 부를 때 새로 띄운다


def test_quiet_recording_names_the_mic(fake, monkeypatch):
    sd, a = fake
    monkeypatch.setattr(assistant, "_has_speech", lambda pcm: False)
    take = assistant._Take()
    take.mic = "MacBook Pro 마이크"
    take.chunks = [np.full((1600, 1), 30, dtype=np.int16)]
    a._answer(take)
    assert a.state["state"] == "error"
    assert "'MacBook Pro 마이크' 소리가 작습니다" in a.state["a"]


def test_windows_follows_system_mic_even_bluetooth(monkeypatch):
    sd = FakeSd(["Microsoft Sound Mapper - Input", "마이크 배열(Realtek(R) Audio)", "헤드셋 마이크(AirPods Hands-Free)"],
                default=0, loud=[])
    a = make(monkeypatch, sd, "win32", "헤드셋 마이크(AirPods Hands-Free)")
    assert a._mic_order() == ["헤드셋 마이크(AirPods Hands-Free)", "마이크 배열(Realtek(R) Audio)"]


def test_windows_mme_name_cut_at_31_chars_matches():
    full = "Headset Microphone (WH-1000XM4 Hands-Free AG Audio)"
    assert assistant._match_input(full, ["Microphone Array (Realtek(R) A", full[:31]]) == full[:31]
    assert assistant._match_input("Mic", ["Microphone"]) is None


def test_windows_order_uses_real_default_and_skips_aliases(monkeypatch):
    sd = FakeSd(["Microsoft Sound Mapper - Input", "마이크 배열(Realtek(R) Audio)", "헤드셋 마이크(USB Audio)",
                 "Primary Sound Capture Driver"], default=0, loud=[])
    a = make(monkeypatch, sd, "win32", "헤드셋 마이크(USB Audio)")
    assert a._mic_order() == ["헤드셋 마이크(USB Audio)", "마이크 배열(Realtek(R) Audio)"]


def test_windows_order_falls_back_to_sound_mapper(monkeypatch):
    sd = FakeSd(["Microsoft Sound Mapper - Input", "마이크 배열(Realtek(R) Audio)"], default=0, loud=[])
    a = make(monkeypatch, sd, "win32", None)  # Core Audio 를 못 읽었다
    assert a._mic_order() == ["Microsoft Sound Mapper - Input", "마이크 배열(Realtek(R) Audio)"]


def test_real_recorder_process_answers():
    """진짜 녹음 프로세스가 뜨고 장치 목록에 답한다 (마이크는 열지 않는다)."""
    mics = assistant.MicClient()
    try:
        names, default = mics.devices()
        assert isinstance(names, list) and isinstance(default, str)
    finally:
        mics.stop()


@pytest.fixture
def phone(monkeypatch):
    # MacBook 마이크는 덮개가 닫혀 조용하고 아이폰 마이크만 소리가 온다
    sd = FakeSd(["MacBook Pro 마이크", "‘내 폰’ 마이크"], default=0, loud=["‘내 폰’ 마이크"])
    a = make(monkeypatch, sd, default="MacBook Pro 마이크")
    monkeypatch.setattr(assistant, "_mac_inputs", lambda: {"MacBook Pro 마이크": (1, "bltn"), "‘내 폰’ 마이크": (2, "ccwl")})
    a._device_signature()
    return sd, a


def test_phone_mic_not_used_until_picked(phone, fallback_file):
    sd, a = phone
    assert a._mic_order() == ["MacBook Pro 마이크"]
    take = record(a, 2.2)
    assert take.failed and take.mic == ""  # 조용한 MacBook 마이크만 써 보고 아이폰은 열지 않는다
    assert not fallback_file.exists()
    status = a.mic_status()
    assert "‘내 폰’ 마이크" in status["list"] and status["limited"] == ["‘내 폰’ 마이크"] and "left" not in status


def test_picked_phone_mic_used_for_30_minutes(phone, mic_files, monkeypatch):
    sd, a = phone
    a.on_event({"mic_pick": "‘내 폰’ 마이크"})
    assert a._mic_order()[0] == "‘내 폰’ 마이크"
    assert a.mic_status()["left"] == 30
    assert record(a, 1.0).mic == "‘내 폰’ 마이크"
    monkeypatch.setattr(a, "_probe_round", lambda: setattr(a, "_probing", False))
    a._phone_until = time.monotonic() - 1  # 30분이 지났다
    a._watched -= 2
    a._watch_mics()
    assert a._picked == "" and not (mic_files / "picked_mic.txt").exists()
    assert "30분" in a.pending_toast and not a._fresh  # 녹음 프로세스를 새로 띄워 아이폰 마이크를 닫는다
    assert a._mic_order() == ["MacBook Pro 마이크"]


def test_phone_pick_left_from_before_restart_is_dropped_quietly(phone, mic_files, monkeypatch):
    sd, a = phone
    (mic_files / "picked_mic.txt").write_text("‘내 폰’ 마이크", encoding="utf-8")
    a._picked = "‘내 폰’ 마이크"  # 도우미를 다시 켜면 허락은 남지 않는다
    monkeypatch.setattr(a, "_probe_round", lambda: setattr(a, "_probing", False))
    a._watch_mics()
    assert a._picked == "" and not a.pending_toast
    assert "‘내 폰’ 마이크" not in a._mic_order()


def test_phone_default_input_kept_only_while_allowed(phone, monkeypatch):
    sd, a = phone
    calls = []
    monkeypatch.setattr(assistant, "_mac_use_builtin_input", lambda allowed="": calls.append(allowed) and None)
    monkeypatch.setattr(a, "_probe_round", lambda: setattr(a, "_probing", False))
    a._watch_mics()
    a.on_event({"mic_pick": "‘내 폰’ 마이크"})
    a._watch_mics()
    assert calls == ["", "‘내 폰’ 마이크"]


def test_phone_mic_remembered_while_its_connection_is_unknown(phone, mic_files, monkeypatch):
    sd, a = phone
    assert "‘내 폰’ 마이크" in (mic_files / "phone_mics.txt").read_text(encoding="utf-8")
    # 다시 붙는 동안은 연결 방식이 비어 있다
    monkeypatch.setattr(assistant, "_mac_inputs", lambda: {"MacBook Pro 마이크": (1, "bltn"), "‘내 폰’ 마이크": (2, "\0\0\0\0")})
    b = make(monkeypatch, sd, default="MacBook Pro 마이크")
    monkeypatch.setattr(assistant, "_mac_inputs", lambda: {"MacBook Pro 마이크": (1, "bltn"), "‘내 폰’ 마이크": (2, "\0\0\0\0")})
    assert b._mic_order() == ["MacBook Pro 마이크"]


def test_phone_found_just_before_choosing(fake, monkeypatch):
    sd, a = fake
    sd.devices.append({"name": "새 폰 마이크", "max_input_channels": 1, "hostapi": 0})
    a.mics.restart()
    monkeypatch.setattr(assistant, "_mac_inputs", lambda: {"USB 마이크": (0, "usb "), "새 폰 마이크": (3, "ccwl")})
    assert "새 폰 마이크" not in a._mic_order()  # 장치가 바뀐 것을 2초마다 보기 전에 녹음해도
