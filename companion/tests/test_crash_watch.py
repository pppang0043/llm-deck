"""기기 크래시 감지와 알림. 시계와 알림·네트워크는 가짜로 바꾼다 (실제 메시지를 보내지 않는다)."""
import threading
import urllib.error
from pathlib import Path

import pytest

import crash_watch
from transports import frames, usb

GURU = "Guru Meditation Error: Core  1 panic'ed (LoadProhibited). Exception was unhandled."
TOKEN = "123456:SECRET-TOKEN"


@pytest.fixture
def watch(monkeypatch):
    """알림과 텔레그램을 기록만 하는 CrashWatch. 시각은 w.now 로 민다."""
    w = crash_watch.CrashWatch()
    w.now, w.notes, w.telegrams = 0.0, [], []
    monkeypatch.setattr(crash_watch.time, "monotonic", lambda: w.now)
    monkeypatch.setattr(crash_watch, "_background", lambda target, *args: target(*args))
    monkeypatch.setattr(crash_watch, "desktop_notify", w.notes.append)
    monkeypatch.setattr(crash_watch, "send_telegram", w.telegrams.append)
    return w


def crash_once(w):
    for line in (GURU, "Backtrace: 0x420061c7:0x3fceb9e0", "ELF file SHA256: 4cf9c244f", "Rebooting..."):
        w.on_log_line(line)


def event(count, build="abc", uptime=42):
    return {"crash": {"reason": "panic", "count": count, "uptime_s": uptime, "build": build}}


@pytest.mark.parametrize("line", [
    GURU, "assert failed: udp_new_ip_type /IDF/components/lwip/lwip/src/core/udp.c:1278 (Required to lock TCPIP core functionality!)",
    "abort() was called at PC 0x40383ed1 on core 1", "E (30123) task_wdt: Task watchdog got triggered.", "Rebooting...",
])
def test_every_crash_line_is_detected(watch, line, capsys):
    watch.on_log_line(line)
    out = capsys.readouterr().out
    assert out.startswith("[crash] ") and "최근 1시간 1번" in out and line[:50] in out
    assert len(watch.notes) == 1


@pytest.mark.parametrize("line", [
    "[health] loop watchdog on (20s)", "[link] event to USB: {\"text\":\"Rebooting...\"}", "rst:0xc (RTC_SW_CPU_RST),boot:0x8",
    "Rebooting... later", "[health] crash test",
])
def test_ordinary_lines_are_ignored(watch, line, capsys):
    watch.on_log_line(line)
    assert capsys.readouterr().out == "" and watch.notes == []


def test_one_panic_counts_once(watch, capsys):
    crash_once(watch)
    assert capsys.readouterr().out.count("[crash]") == 1


def test_fast_boot_loop_counts_each_crash(watch, capsys):
    crash_once(watch)
    watch.now += 3
    crash_once(watch)
    assert capsys.readouterr().out.count("[crash]") == 2


def test_device_event_alone_is_detected(watch, capsys):
    watch.on_event(event(1))
    out = capsys.readouterr().out
    assert "[crash]" in out and "panic" in out and "abc" in out and "42" in out
    assert len(watch.notes) == 1


def test_event_after_usb_lines_is_the_same_crash(watch, capsys):
    crash_once(watch)
    watch.now += 20
    watch.on_event(event(1))
    assert capsys.readouterr().out.count("[crash]") == 1


def test_repeated_event_is_ignored_but_a_new_count_is_not(watch, capsys):
    watch.on_event(event(1))
    watch.on_event(event(1))
    assert capsys.readouterr().out.count("[crash]") == 1
    watch.now += 60
    watch.on_event(event(2))
    assert capsys.readouterr().out.count("[crash]") == 1


def test_count_restarting_after_a_long_survival_is_a_new_crash(watch, capsys):
    """count 는 연속 횟수라 오래 살아남은 뒤에는 다시 1 이다 (crash_log.h)."""
    watch.on_event(event(1, uptime=500))
    watch.now += 7200
    watch.on_event(event(1, uptime=7000))
    assert capsys.readouterr().out.count("[crash]") == 2


def test_jump_in_count_includes_unreported_crashes_in_the_loop_check(watch, capsys):
    """알리기 전에 죽은 부팅들은 다음 알림 하나로 온다. 알림이 한 번이어도 크래시 루프다."""
    watch.on_event(event(1, uptime=10))
    assert watch.telegrams == []
    watch.on_event(event(4, uptime=11))
    assert "최근 1시간 4번" in capsys.readouterr().out
    assert len(watch.telegrams) == 1


def test_late_event_is_a_new_crash(watch, capsys):
    crash_once(watch)
    watch.now += crash_watch.EVENT_MERGE_SECONDS
    watch.on_event(event(1))
    assert capsys.readouterr().out.count("[crash]") == 2


@pytest.mark.parametrize("bad", [None, "x", {"fw_proto": 6}, {"crash": "panic"}, {"crash": None}])
def test_events_without_crash_are_tolerated(watch, bad, capsys):
    watch.on_event(bad)
    assert capsys.readouterr().out == ""


def test_partial_crash_event_is_tolerated(watch, capsys):
    watch.on_event({"crash": {}})
    assert "[crash]" in capsys.readouterr().out


def test_hour_count_forgets_old_crashes(watch, capsys):
    crash_once(watch)
    watch.now += 3599
    crash_once(watch)
    assert "최근 1시간 2번" in capsys.readouterr().out
    watch.now += 1
    crash_once(watch)
    assert "최근 1시간 2번" in capsys.readouterr().out


def test_desktop_notifications_are_spaced_but_log_is_not(watch, capsys):
    for _ in range(3):
        crash_once(watch)
        watch.now += 60
    assert capsys.readouterr().out.count("기기가 오류로 재시작했습니다") == 3
    assert len(watch.notes) == 1
    watch.now += crash_watch.NOTIFY_GAP
    crash_once(watch)
    assert len(watch.notes) == 2


def test_two_crashes_are_not_a_loop(watch):
    crash_once(watch)
    watch.now += 60
    crash_once(watch)
    assert watch.telegrams == []


def test_three_spread_out_crashes_are_not_a_loop(watch):
    for _ in range(3):
        crash_once(watch)
        watch.now += 301
    assert watch.telegrams == []


def test_third_crash_in_ten_minutes_sends_telegram_once(watch):
    for _ in range(3):
        crash_once(watch)
        watch.now += 60
    assert len(watch.telegrams) == 1 and "크래시 루프" in watch.telegrams[0]
    for _ in range(5):
        crash_once(watch)
        watch.now += 60
    assert len(watch.telegrams) == 1


def test_telegram_rate_limit_allows_one_per_hour(watch):
    for _ in range(3):
        crash_once(watch)
        watch.now += 60
    watch.now += crash_watch.TELEGRAM_GAP
    for _ in range(3):
        crash_once(watch)
        watch.now += 60
    assert len(watch.telegrams) == 2


def test_loop_is_detected_from_device_events_alone(watch):
    for count in (1, 2, 3):
        watch.on_event(event(count))
        watch.now += 60
    assert len(watch.telegrams) == 1


def _env_file(text):
    path = Path.home() / ".secrets/telegram.env"
    path.parent.mkdir()
    path.write_text(text)


def test_telegram_skipped_without_file(monkeypatch, capsys):
    monkeypatch.setattr(crash_watch.urllib.request, "urlopen", lambda *a, **kw: pytest.fail("네트워크 호출"))
    crash_watch.send_telegram("x")
    assert capsys.readouterr().out == ""


def test_telegram_request_has_timeout_and_reads_env_at_runtime(monkeypatch):
    _env_file(f"# 설명\nTELEGRAM_BOT_TOKEN={TOKEN}\nexport TELEGRAM_CHAT_ID=\"42\"\n")
    calls = []

    class Reply:
        def close(self): pass

    monkeypatch.setattr(crash_watch.urllib.request, "urlopen", lambda url, data, timeout: calls.append((url, data, timeout)) or Reply())
    crash_watch.send_telegram("hello")
    (url, data, timeout), = calls
    assert url == f"https://api.telegram.org/bot{TOKEN}/sendMessage"
    assert b"chat_id=42" in data and b"text=hello" in data and timeout <= 15


@pytest.mark.parametrize("error", [
    urllib.error.URLError(f"https://api.telegram.org/bot{TOKEN}/sendMessage"),
    urllib.error.HTTPError(f"https://api.telegram.org/bot{TOKEN}/sendMessage", 401, "no", {}, None),
])
def test_telegram_failure_is_logged_without_token(monkeypatch, capsys, error):
    _env_file(f"TELEGRAM_BOT_TOKEN={TOKEN}\nTELEGRAM_CHAT_ID=42\n")

    def urlopen(*a, **kw):
        raise error

    monkeypatch.setattr(crash_watch.urllib.request, "urlopen", urlopen)
    crash_watch.send_telegram("hello")
    captured = capsys.readouterr()
    assert "[crash]" in captured.out and "SECRET" not in captured.out + captured.err


def test_alerts_do_not_run_on_the_receive_thread(monkeypatch):
    w = crash_watch.CrashWatch()
    done = threading.Event()
    threads = []

    def record(text):
        threads.append(threading.current_thread())
        done.set()

    monkeypatch.setattr(crash_watch, "desktop_notify", record)
    w.on_log_line(GURU)
    assert done.wait(2)
    assert threads[0] is not threading.current_thread()


def test_macos_notification_passes_text_as_argument(monkeypatch):
    monkeypatch.setattr(crash_watch.sys, "platform", "darwin")
    calls = []
    monkeypatch.setattr(crash_watch.subprocess, "run", lambda args, **kw: calls.append((args, kw)))
    crash_watch.desktop_notify('따옴표 " 포함')
    (args, kw), = calls
    assert args[0] == "osascript" and args[-1] == '따옴표 " 포함' and args[-2] == "--"
    assert not any('따옴표' in part for part in args[:-1]) and kw["timeout"] <= 15


def test_failed_notification_is_only_logged(monkeypatch, capsys):
    monkeypatch.setattr(crash_watch.sys, "platform", "darwin")

    def run(*a, **kw):
        raise OSError("no osascript")

    monkeypatch.setattr(crash_watch.subprocess, "run", run)
    crash_watch.desktop_notify("x")
    assert "[crash]" in capsys.readouterr().out


def test_usb_log_lines_reach_the_sink_and_sink_errors_stay_out_of_the_reader(monkeypatch, capsys):
    seen = []
    monkeypatch.setattr(frames, "log_sink", seen.append)
    rest = usb.UsbTransport._print_log(b"Rebooting...\r\nnot finis", False)
    assert seen == ["Rebooting..."] and rest == b"not finis"

    def broken(text):
        raise ValueError("sink")

    monkeypatch.setattr(frames, "log_sink", broken)
    usb.UsbTransport._print_log(b"line\n", False)  # 받는 스레드로 번지지 않는다
    assert "기기> line" in capsys.readouterr().out
