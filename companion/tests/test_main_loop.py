"""main.py 의 기기 바퀴: 첫 메시지, 한 번짜리 명령의 끝내기, 끊김 복구, 연결 갈아타기, 작은 메시지.

실제 main() 을 처음부터 돌린다. 부품만 가짜이고 time.sleep 이 바퀴를 세어 끝낸다.
"""

import sys

import pytest

import main
from transports import frames
from transports.commands import CommandError


class Stop(BaseException):
    """무한 바퀴를 끝낸다. main() 의 except Exception 에 잡히지 않는다."""


class Part:
    """on_event 와 pending_toast 만 있으면 되는 부품."""

    def __init__(self, *a, **k):
        self.pending_toast = None
        self.events = []

    def on_event(self, event):
        self.events.append(event)


class FakeAssistant(Part):
    def __init__(self, *a, **k):
        super().__init__()
        self.mic = True
        self.status = {"state": "ok"}
        self.answer = None
        self.orca = None
        self.audio = []

    def mic_status(self):
        return self.status

    def pending(self):
        answer, self.answer = self.answer, None
        return answer

    def on_device_audio(self, seq, pcm):
        self.audio.append((seq, pcm))


class FakeOrca(Part):
    def __init__(self, *a, **k):
        super().__init__()
        self.resends = 0
        self.state = None

    def resend(self):
        self.resends += 1

    def pending(self):
        state, self.state = self.state, None
        return state


class FakePhotos(Part):
    def __init__(self, *a, **k):
        super().__init__()
        self.pending_scan = None
        self.pending_roi = None
        self.buf = None
        self.name = "photo.jpg"

    def on_chunk(self, *a):
        pass


class FakeVideos(Part):
    def __init__(self, *a, **k):
        super().__init__()
        self.dir = "videos"
        self.writer = None
        self.finished = 0

    def finish(self):
        self.finished += 1

    def on_chunk(self, *a):
        pass


class FakePc(Part):
    def __init__(self, *a, **k):
        super().__init__()
        self.stats = None

    def due(self):
        return self.stats is not None

    def collect(self):
        stats, self.stats = self.stats, None
        return stats


class FakePeer(Part):
    def __init__(self, *a, **k):
        super().__init__()
        self.proto = 6
        self.resets = 0
        self.sends = 0

    def reset(self):
        self.resets += 1

    def sent(self):
        self.sends += 1


class FakeMirror:
    def __init__(self, cfg, get_transport):
        self.get_transport = get_transport
        self.up = []
        self.lost = 0
        self.extra = None

    def on_frame(self, body):
        pass

    def link_lost(self):
        self.lost += 1

    def link_up(self, name):
        self.up.append(name)

    def pending(self):
        extra, self.extra = self.extra, None
        return extra


class FakeCollector:
    def __init__(self, cfg):
        self.limits_only = []

    def start(self, limits_only=False):
        self.limits_only.append(limits_only)

    def take(self, wait):
        return {"updated": "12:00", "services": [{"name": "x"}], "host": "pc", "now": 1}


class FakeTransport:
    def __init__(self, name="USB", fail_sends=0, send_error=None, connected=True, confirmed=True):
        self.name = name
        self.sent = []
        self.closed = 0
        self.fail_sends = fail_sends
        self.send_error = send_error
        self.is_connected = connected
        self.confirmed = confirmed
        self.on_confirm = None
        self.last_rx = 1.0
        self.opened_at = 0.0
        self.opens = 0

    def open(self):
        self.opens += 1
        return True

    def send(self, message):
        if self.send_error:
            raise self.send_error
        if self.fail_sends:
            self.fail_sends -= 1
            raise OSError("broken pipe")
        self.sent.append(message)

    def close(self):
        self.closed += 1

    def connected(self):
        return self.is_connected

    def take_confirm(self):
        self.on_confirm()


class Rig:
    """가짜 부품을 끼운 main(). parts 에 main() 이 만든 부품이 쌓인다."""

    def __init__(self, monkeypatch):
        self.monkeypatch = monkeypatch
        self.parts = {}
        self.transports = []
        self.connects = []
        self.sleeps = []
        self.max_sleeps = 200
        self.plugged = False
        self.on_sleep = None
        for path, fake in {
            (main.audio, "Player"): Part, (main.sounds, "Sounds"): Part, (main.launcher, "Launcher"): Part,
            (main.typer, "Typer"): Part, (main.crash_watch, "CrashWatch"): CrashPart,
            (main.assistant, "Assistant"): FakeAssistant, (main.orca, "Orca"): FakeOrca,
            (main.photos, "PhotoReceiver"): FakePhotos, (main.videos, "VideoReceiver"): FakeVideos,
            (main.pcstats, "PcStats"): FakePc, (main.version, "Peer"): FakePeer,
            (main.mirror, "Mirror"): FakeMirror, (main, "Collector"): FakeCollector,
            (main, "Watchdog"): WatchdogPart, (main.audio, "Recorder"): RecorderPart,
        }.items():
            monkeypatch.setattr(path[0], path[1], self._keep(path[1], fake))
        monkeypatch.setattr(main.typer, "configure", lambda items: None)
        monkeypatch.setattr(main.typer, "ITEMS", ["kw"])
        monkeypatch.setattr(main, "claim_device", lambda: True)
        monkeypatch.setattr(main, "load_config", lambda: {})
        monkeypatch.setattr(main, "_private_files", lambda: None)
        monkeypatch.setattr(main, "_host_name", lambda: "pc")
        monkeypatch.setattr(main.faulthandler, "register", lambda *a, **k: None, raising=False)
        monkeypatch.setattr(main, "connect", self._connect)
        monkeypatch.setattr(main, "usb_plugged", lambda cfg: self.plugged)
        monkeypatch.setattr(main.time, "sleep", self._sleep)
        monkeypatch.setattr(frames, "log_sink", None, raising=False)
        monkeypatch.setattr(frames, "mirror_sink", None, raising=False)

    def _keep(self, name, fake):
        def build(*a, **k):
            part = fake(*a, **k)
            self.parts[name] = part
            return part
        return build

    def _connect(self, names, cfg, on_audio, on_event, on_photo, on_video):
        self.connects.append(names)
        self.on_event = on_event
        self.on_audio = on_audio
        if not self.transports:
            return None
        return self.transports.pop(0)

    def _sleep(self, seconds):
        self.sleeps.append(seconds)
        if self.on_sleep:
            self.on_sleep(len(self.sleeps))
        if len(self.sleeps) >= self.max_sleeps:
            raise Stop

    def run(self, *argv, ticks=None):
        if ticks is not None:
            self.max_sleeps = ticks
        self.monkeypatch.setattr(sys, "argv", ["main.py", *argv])
        try:
            main.main()
        except Stop:
            return "looping"
        return "finished"


class CrashPart(Part):
    def on_log_line(self, line):
        pass


class WatchdogPart:
    def __init__(self):
        self.beats = 0

    def beat(self):
        self.beats += 1


class RecorderPart(Part):
    def __init__(self, path):
        super().__init__()
        self.path = path
        self.closed = 0

    def close(self):
        self.closed += 1


@pytest.fixture
def rig(monkeypatch):
    return Rig(monkeypatch)


# --- 첫 메시지 ---


def test_usage_message_carries_status_fields(rig):
    usb = FakeTransport()
    rig.transports = [usb]
    assert rig.run(ticks=1) == "looping"
    first = usb.sent[0]
    assert first["services"] == [{"name": "x"}]
    assert first["ask_mic"] is True and first["pc_mic"] == {"state": "ok"} and first["keywords"] == ["kw"]
    assert "ask_fw" not in first
    assert rig.parts["Peer"].sends == 1


def test_usage_asks_firmware_version_while_unknown(rig, monkeypatch):
    usb = FakeTransport()
    rig.transports = [usb]

    class Unknown(FakePeer):
        def __init__(self):
            super().__init__()
            self.proto = None

    monkeypatch.setattr(main.version, "Peer", Unknown)
    rig.run(ticks=1)
    assert usb.sent[0]["ask_fw"] is True


@pytest.mark.parametrize("argv, expected", [
    (["--mic", "on"], {"mic": True}),
    (["--mic", "off"], {"mic": False}),
    (["--test-photo"], {"photo_test": True}),
    (["--camera", "scan"], {"camera": "scan"}),
    (["--links", "usb=on,ble=off"], {"links": {"usb": True, "ble": False}}),
    (["--view", "home"], {"view": "home"}),
    (["--screenshot"], {"screenshot": True}),
    (["--sd-pull"], {"sd_pull": True}),
    (["--sd-clear"], {"sd_clear": True}),
    (["--bt-check"], {"bt_check": True}),
    (["--bt-check", "all"], {"bt_check": "all"}),
    (["--rotate"], {"rotate": True}),
    (["--saver"], {"saver": True}),
    (["--test-video", "5"], {"camera": "record"}),
    (["--camera", "open", "--test-video", "5"], {"camera": "record"}),
    (["--camera-mount", "1m"], {"camera_mount": {"rot": 1, "mirror": True}}),
    (["--camera-mount", "3"], {"camera_mount": {"rot": 3, "mirror": False}}),
    (["--camera-ctl", "sharp=2,special=1"], {"camera_ctl": {"sharp": 2, "special": 1}}),
    (["--battery", "4.12"], {"battery_mv": 4120}),
    (["--battery", "0"], {"battery_mv": 0}),
])
def test_one_shot_fields_go_on_the_first_message(rig, argv, expected):
    usb = FakeTransport()
    rig.transports = [usb]
    rig.run(*argv, ticks=1)
    first = usb.sent[0]
    assert {key: first[key] for key in expected} == expected


def test_one_shot_fields_only_on_the_first_message(rig):
    usb = FakeTransport()
    rig.transports = [usb]
    # --mic 은 명령을 보내고도 끝나지 않는다. 다음 사용량 메시지에는 다시 싣지 않는다
    rig.run("--mic", "on", ticks=main.INTERVAL * 2 + 2)
    usage = [m for m in usb.sent if "services" in m]
    assert len(usage) == 2
    assert usage[0]["mic"] is True and "mic" not in usage[1]


def test_failed_first_send_keeps_one_shot_fields_for_the_retry(rig):
    bad, good = FakeTransport(fail_sends=1), FakeTransport()
    rig.transports = [bad, good]
    rig.run("--mic", "on", ticks=3)
    assert good.sent[0]["mic"] is True


# --- 한 번짜리 명령은 끝난다 ---


@pytest.mark.parametrize("argv, wait", [
    (["--camera", "open"], 2), (["--camera", "close"], 2), (["--camera", "gallery"], 2),
    (["--camera-mount", "1"], 2), (["--camera-ctl", "sharp=1"], 2), (["--links", "ble=off"], 2),
    (["--view", "home"], 1), (["--saver"], 1), (["--sd-clear"], 1), (["--rotate"], 1),
    (["--battery", "4.0"], 1),
])
def test_quick_commands_finish_and_close(rig, argv, wait):
    usb = FakeTransport()
    rig.transports = [usb]
    assert rig.run(*argv) == "finished"
    assert usb.closed == 1 and rig.sleeps == [wait]


@pytest.mark.parametrize("argv", [
    ["--test-photo"], ["--screenshot"], ["--sd-pull"], ["--camera", "shoot"], ["--camera", "dump"],
    ["--view", "home", "--screenshot"],
])
def test_photo_commands_wait_for_the_photo(rig, argv):
    usb = FakeTransport()
    rig.transports = [usb]

    def arrive(count):
        if count == 3:
            rig.parts["PhotoReceiver"].buf = None
            rig.parts["PhotoReceiver"].name = "shot.jpg"

    rig.on_sleep = arrive
    # 받기 시작한 상태에서 출발한다
    original = FakePhotos.__init__

    def receiving(self, *a, **k):
        original(self, *a, **k)
        self.buf = bytearray(b"x")

    rig.monkeypatch.setattr(FakePhotos, "__init__", receiving)
    assert rig.run(*argv) == "finished"
    assert usb.closed == 1 and len(rig.sleeps) == 3


def test_photo_wait_gives_up_after_a_minute(rig):
    usb = FakeTransport()
    rig.transports = [usb]
    original = FakePhotos.__init__

    def stuck(self, *a, **k):
        original(self, *a, **k)
        self.buf = bytearray(b"x")

    rig.monkeypatch.setattr(FakePhotos, "__init__", stuck)
    assert rig.run("--camera", "shoot") == "finished"
    assert usb.closed == 1 and len(rig.sleeps) == 60


def test_test_video_records_then_stops(rig):
    usb = FakeTransport()
    rig.transports = [usb]
    assert rig.run("--test-video", "5") == "finished"
    assert [m.get("camera") for m in usb.sent] == ["record", "record", "close"]
    assert rig.sleeps[0] == 5 and usb.closed == 1


@pytest.mark.parametrize("argv, wait", [(["--bt-check"], 20), (["--bt-check", "all"], 120)])
def test_bt_check_waits_for_the_scan(rig, argv, wait):
    usb = FakeTransport()
    rig.transports = [usb]
    assert rig.run(*argv) == "finished"
    assert rig.sleeps == [wait] and usb.closed == 1


def test_record_saves_and_finishes(rig):
    usb = FakeTransport()
    rig.transports = [usb]
    assert rig.run("--record", "a.wav") == "finished"
    assert rig.sleeps == [main.RECORD_SECONDS] and usb.closed == 1
    assert rig.parts["Recorder"].closed == 1 and rig.parts["Recorder"].path == "a.wav"


def test_record_without_a_device_also_finishes(rig):
    assert rig.run("--record", "a.wav") == "finished"
    assert rig.parts["Recorder"].closed == 1


@pytest.mark.parametrize("argv", [["--mic", "on"], ["--camera", "scan"], []])
def test_other_commands_keep_running(rig, argv):
    usb = FakeTransport()
    rig.transports = [usb]
    assert rig.run(*argv, ticks=5) == "looping"
    assert usb.closed == 0


def test_one_shot_waits_while_device_is_missing(rig):
    # 기기를 찾지 못했으면 명령을 끝내지 않고 계속 찾는다
    assert rig.run("--view", "home", ticks=3) == "looping"
    assert len(rig.connects) == 3 and rig.sleeps == [2, 2, 2]


# --- 연결 관리 ---


def test_connect_announces_link_and_resets_session(rig):
    usb = FakeTransport()
    rig.transports = [usb]
    rig.run(ticks=1)
    assert rig.parts["Mirror"].up == ["USB"]
    assert rig.parts["Orca"].resends == 1 and rig.parts["Peer"].resets == 1
    assert rig.parts["VideoReceiver"].finished == 1 and rig.parts["Mirror"].lost == 1


def test_unconfirmed_usb_announces_only_after_confirm(rig):
    usb = FakeTransport(confirmed=False)
    rig.transports = [usb]
    rig.run(ticks=1)
    assert rig.parts["Mirror"].up == []
    usb.on_confirm()
    assert rig.parts["Mirror"].up == ["USB"]


def test_mirror_reads_the_current_transport(rig):
    usb = FakeTransport()
    rig.transports = [usb]
    rig.run(ticks=1)
    assert rig.parts["Mirror"].get_transport() is usb


def test_no_device_keeps_sharing_limits_only(rig):
    rig.run(ticks=2)
    assert rig.parts["Collector"].limits_only == [True, True]
    assert rig.sleeps == [2, 2]


def test_send_failure_closes_and_reconnects(rig):
    bad, good = FakeTransport(fail_sends=1), FakeTransport()
    rig.transports = [bad, good]
    rig.run(ticks=3)
    assert bad.closed == 1 and good.sent
    assert rig.sleeps[0] == 2  # 끊긴 바퀴는 잠깐 쉬고 다시 붙는다
    assert len(rig.connects) == 2


def test_lost_connection_during_wait_reconnects(rig):
    first, second = FakeTransport(), FakeTransport()
    rig.transports = [first, second]

    def unplug(count):
        if count == 2:
            first.is_connected = False

    rig.on_sleep = unplug
    rig.run(ticks=5)
    assert first.closed == 1 and second.sent
    assert rig.parts["Orca"].resends == 2


def test_command_error_stops_the_helper_without_retry(rig):
    usb = FakeTransport(send_error=CommandError("uncertain"))
    rig.transports = [usb]
    with pytest.raises(SystemExit) as stop:
        rig.run("--view", "home")
    assert "uncertain" in str(stop.value)
    assert usb.closed == 1 and len(rig.connects) == 1


def test_unexpected_error_waits_and_starts_over(rig):
    usb = FakeTransport(send_error=ValueError("bug"))
    good = FakeTransport()
    rig.transports = [usb, good]
    rig.run(ticks=2)
    assert usb.closed == 1 and rig.sleeps[0] == 10 and good.sent


def test_close_failure_during_recovery_is_ignored(rig):
    usb = FakeTransport(send_error=ValueError("bug"))

    def broken_close():
        raise OSError("already gone")

    usb.close = broken_close
    good = FakeTransport()
    rig.transports = [usb, good]
    rig.run(ticks=2)
    assert good.sent


def test_usb_cable_replaces_a_wireless_link(rig):
    wifi, usb = FakeTransport("Wi-Fi"), FakeTransport("USB")
    rig.transports = [wifi, usb]
    rig.plugged = True
    rig.run(ticks=1)
    assert wifi.closed == 1 and usb.sent


def test_usb_is_not_replaced_when_a_link_is_forced(rig):
    wifi = FakeTransport("Wi-Fi")
    rig.transports = [wifi]
    rig.plugged = True
    rig.run("--link", "wifi", ticks=1)
    assert wifi.closed == 0 and rig.connects == [["wifi"]]


def test_bluetooth_upgrades_to_wifi_when_found(rig, monkeypatch):
    ble = FakeTransport("Bluetooth")
    rig.transports = [ble]
    found = FakeTransport("Wi-Fi")
    monkeypatch.setitem(main.TRANSPORTS, "wifi", lambda *a: found)
    monkeypatch.setattr(main, "CommandTransport", lambda raw: raw)
    now = [1000.0]
    monkeypatch.setattr(main.time, "time", lambda: now[0])

    def later(count):
        now[0] += main.WIFI_RETRY + 1

    # 처음 메시지를 보낸 뒤 시간이 지나 다음 차례에 Wi-Fi 를 찾는다
    rig.on_sleep = later
    rig.run(ticks=main.INTERVAL * 2 + 2)
    assert ble.closed == 1 and found.sent
    assert rig.parts["Mirror"].up == ["Bluetooth", "Wi-Fi"]
    assert rig.parts["Peer"].resets == 2 and rig.parts["Orca"].resends == 2


def test_bluetooth_stays_when_wifi_is_not_found(rig, monkeypatch):
    ble = FakeTransport("Bluetooth")
    rig.transports = [ble]
    missing = FakeTransport("Wi-Fi")
    missing.open = lambda: False
    monkeypatch.setitem(main.TRANSPORTS, "wifi", lambda *a: missing)
    now = [1000.0]
    monkeypatch.setattr(main.time, "time", lambda: now[0])

    def later(count):
        now[0] += main.WIFI_RETRY + 1

    rig.on_sleep = later
    rig.run(ticks=main.INTERVAL * 2 + 2)
    assert ble.closed == 0 and missing.closed == 1


# --- 사이사이의 작은 메시지 ---


def _small_messages(usb):
    return [m for m in usb.sent if "services" not in m]


def test_quiet_tick_sends_nothing_extra(rig):
    usb = FakeTransport()
    rig.transports = [usb]
    rig.run(ticks=4)
    assert _small_messages(usb) == []


def test_small_updates_are_forwarded_with_host(rig):
    usb = FakeTransport()
    rig.transports = [usb]

    def fill(count):
        if count == 1:
            rig.parts["PcStats"].stats = {"cpu": 1}
            rig.parts["Assistant"].answer = {"state": "thinking"}
            rig.parts["Orca"].state = {"rows": []}
            rig.parts["PhotoReceiver"].pending_scan = "code"
            rig.parts["PhotoReceiver"].pending_roi = [1, 2, 3, 4]
            rig.parts["Mirror"].extra = {"mirror": True}

    rig.on_sleep = fill
    rig.run(ticks=3)
    assert _small_messages(usb)[0] == {
        "pc": {"cpu": 1}, "ask": {"state": "thinking"}, "orca": {"rows": []}, "scan": "code",
        "scan_roi": [1, 2, 3, 4], "mirror": True, "host": "pc",
    }
    assert rig.parts["PhotoReceiver"].pending_scan is None and rig.parts["PhotoReceiver"].pending_roi is None


def test_empty_scan_roi_is_still_sent(rig):
    # [] 는 "바코드 자리를 못 찾음" 이라는 뜻이다
    usb = FakeTransport()
    rig.transports = [usb]

    def fill(count):
        if count == 1:
            rig.parts["PhotoReceiver"].pending_roi = []

    rig.on_sleep = fill
    rig.run(ticks=3)
    assert _small_messages(usb) == [{"scan_roi": [], "host": "pc"}]


def test_toasts_from_any_source_are_forwarded_and_cleared(rig):
    usb = FakeTransport()
    rig.transports = [usb]

    def fill(count):
        if count == 1:
            rig.parts["Launcher"].pending_toast = "launcher"

    rig.on_sleep = fill
    rig.run(ticks=3)
    assert _small_messages(usb) == [{"toast": "launcher", "host": "pc"}]
    assert rig.parts["Launcher"].pending_toast is None


def test_two_toasts_at_once_are_both_delivered(rig):
    usb = FakeTransport()
    rig.transports = [usb]

    def fill(count):
        if count == 1:
            rig.parts["Launcher"].pending_toast = "launcher"
            rig.parts["Assistant"].pending_toast = "assistant"

    rig.on_sleep = fill
    rig.run(ticks=4)
    assert [m["toast"] for m in _small_messages(usb)] == ["launcher", "assistant"]


def test_mic_changes_are_sent_once(rig):
    usb = FakeTransport()
    rig.transports = [usb]

    def change(count):
        if count == 1:
            rig.parts["Assistant"].mic = False
            rig.parts["Assistant"].status = {"state": "none"}

    rig.on_sleep = change
    rig.run(ticks=4)
    assert _small_messages(usb) == [{"ask_mic": False, "pc_mic": {"state": "none"}, "host": "pc"}]


def test_small_send_failure_drops_the_link(rig):
    first, second = FakeTransport(), FakeTransport()
    rig.transports = [first, second]

    def fill(count):
        if count == 1:
            rig.parts["Launcher"].pending_toast = "x"
            first.fail_sends = 1

    rig.on_sleep = fill
    rig.run(ticks=4)
    assert first.closed == 1 and second.sent


def test_wait_beats_the_watchdog_every_tick(rig):
    usb = FakeTransport()
    rig.transports = [usb]
    rig.run(ticks=4)
    assert rig.parts["Watchdog"].beats >= 4


# --- 받는 쪽 ---


def test_event_reaches_every_part_even_if_one_fails(rig, capsys):
    usb = FakeTransport()
    rig.transports = [usb]
    rig.run(ticks=1)

    def broken(event):
        raise RuntimeError("bug")

    rig.parts["Sounds"].on_event = broken
    rig.on_event({"sound": "mic_on"})
    for name in ("VideoReceiver", "PhotoReceiver", "Launcher", "Typer", "PcStats", "Assistant", "Orca", "Peer", "CrashWatch"):
        assert rig.parts[name].events == [{"sound": "mic_on"}], name


@pytest.mark.parametrize("code, text", [("not_queued", "받지 못했습니다"), ("not_owner", "다른 PC")])
def test_link_errors_are_explained(rig, capsys, code, text):
    usb = FakeTransport()
    rig.transports = [usb]
    rig.run(ticks=1)
    capsys.readouterr()
    rig.on_event({"link_error": code})
    assert text in capsys.readouterr().out


def test_device_audio_goes_to_sink_and_assistant(rig):
    usb = FakeTransport()
    rig.transports = [usb]
    pushed = []
    rig.parts.clear()
    original = Part.__init__

    def with_push(self, *a, **k):
        original(self, *a, **k)
        self.push = lambda seq, pcm: pushed.append((seq, pcm))

    rig.monkeypatch.setattr(Part, "__init__", with_push)
    rig.run(ticks=1)
    rig.on_audio(1, b"pcm")
    assert pushed == [(1, b"pcm")] and rig.parts["Assistant"].audio == [(1, b"pcm")]


def test_log_and_mirror_sinks_are_connected(rig):
    usb = FakeTransport()
    rig.transports = [usb]
    rig.run(ticks=1)
    assert frames.log_sink == rig.parts["CrashWatch"].on_log_line
    assert frames.mirror_sink == rig.parts["Mirror"].on_frame


# --- 기기 없이 끝나는 명령 ---


def test_command_status_prints_and_closes(rig, capsys):
    usb = FakeTransport()
    usb.command_status = lambda acknowledge=False: {"state": "none", "acknowledged": acknowledge}
    rig.transports = [usb]
    assert rig.run("--acknowledge-command") == "finished"
    assert '"acknowledged": true' in capsys.readouterr().out and usb.closed == 1


def test_command_status_without_device_fails(rig):
    with pytest.raises(SystemExit) as stop:
        rig.run("--command-status")
    assert stop.value.code == 1


def test_second_helper_explains_and_stops(rig, monkeypatch, capsys):
    monkeypatch.setattr(main, "claim_device", lambda: False)
    assert rig.run() == "finished"
    assert "이미 다른 도우미" in capsys.readouterr().out
    assert rig.connects == []
