"""가짜 기록·가짜 시계로 소크 판정을 확인한다. 실제 기기로 요청을 보내지 않는다."""
import errno
import io
import sys
import urllib.error
from pathlib import Path

import pytest

SCRIPTS = Path(__file__).resolve().parents[2] / "firmware/scripts"
if not (SCRIPTS / "soak_check.py").exists():
    pytest.skip("펌웨어 스크립트가 없는 배포본", allow_module_level=True)
sys.path.insert(0, str(SCRIPTS))
import soak_check

BUILD = "abcdef0123456789abcd"
GOOD = f"[version] 기기 펌웨어 Nov  1 2026 (proto 6, build {BUILD})\n"


@pytest.fixture
def clock(monkeypatch, tmp_path):
    """sleep 할 때마다 시계를 밀고, 그 시각에 정해 둔 줄을 기록에 더한다."""
    class Clock:
        now = 0.0
        log = tmp_path / "companion.log"
        later = {}

        def sleep(self, seconds):
            self.now += seconds
            with self.log.open("a") as f:
                f.write(self.later.pop(self.now, ""))

    c = Clock()
    c.log.write_text("  기기> Guru Meditation Error: 올리기 전의 옛 크래시\n  기기> [net] connected, ip 192.0.2.7\n")
    monkeypatch.setattr(soak_check.time, "monotonic", lambda: c.now)
    monkeypatch.setattr(soak_check.time, "sleep", c.sleep)
    monkeypatch.setattr(soak_check, "send_bad_auth", lambda ip: pytest.fail("요청을 보냄"))
    return c


def test_passes_when_build_appears_and_old_crash_is_before_offset(clock):
    offset = clock.log.stat().st_size
    clock.later = {5.0: GOOD}
    assert soak_check.soak(clock.log, offset, 10, BUILD) == []
    assert clock.now >= 10  # 끝까지 지켜본다


def test_fails_with_the_matching_line_on_crash(clock):
    offset = clock.log.stat().st_size
    clock.later = {3.0: GOOD, 7.0: "  기기> assert failed: udp_new_ip_type\n"}
    problems = soak_check.soak(clock.log, offset, 10, BUILD)
    assert len(problems) == 1 and "assert failed: udp_new_ip_type" in problems[0]


def test_fails_on_helper_crash_line_from_device_event(clock):
    offset = clock.log.stat().st_size
    clock.later = {3.0: GOOD + "[crash] 기기가 오류로 재시작했습니다 (최근 1시간 1번). 기기 알림: panic\n"}
    assert soak_check.soak(clock.log, offset, 10, BUILD)


def test_fails_when_expected_build_never_appears(clock):
    offset = clock.log.stat().st_size
    clock.later = {3.0: "[version] 기기 펌웨어 (proto 6, build otherbuild)\n"}
    problems = soak_check.soak(clock.log, offset, 10, BUILD)
    assert len(problems) == 1 and BUILD in problems[0]


def test_helper_messages_about_crashes_do_not_fail(clock):
    offset = clock.log.stat().st_size
    clock.later = {3.0: GOOD + "  기기> [health] crash count 0\n  기기> [link] event to USB: {\"t\":\"Rebooting...\"}\n"
                        "[device] Rebooting... 라고 적은 도우미 줄\n"}
    assert soak_check.soak(clock.log, offset, 10, BUILD) == []


def test_rotated_log_is_read_from_the_start(clock):
    offset = clock.log.stat().st_size + 1000
    clock.log.write_text("새 기록\n")
    clock.later = {2.0: GOOD}
    assert soak_check.soak(clock.log, offset, 5, BUILD) == []


def test_probe_uses_last_known_ip_and_retries_until_answered(clock, monkeypatch, capsys):
    sent = []

    def send(ip):
        sent.append(ip)
        return (len(sent) >= 2, None if len(sent) >= 2 else "대답 없음")

    monkeypatch.setattr(soak_check, "send_bad_auth", send)
    assert soak_check.soak(clock.log, clock.log.stat().st_size, 20, probe=True) == []
    assert sent == ["192.0.2.7", "192.0.2.7"]
    assert "401 bad" in capsys.readouterr().out


def test_probe_never_answered_fails(clock, monkeypatch):
    monkeypatch.setattr(soak_check, "send_bad_auth", lambda ip: (False, "대답 없음"))
    assert soak_check.soak(clock.log, clock.log.stat().st_size, 10, probe=True) == ["대답 없음"]


def test_probe_wrong_answer_fails_at_once(clock, monkeypatch):
    monkeypatch.setattr(soak_check, "send_bad_auth", lambda ip: (True, "HTTP 200"))
    assert soak_check.soak(clock.log, clock.log.stat().st_size, 10, probe=True) == ["HTTP 200"]
    assert clock.now == 0


def test_probe_without_ip_warns_only(clock, capsys):
    clock.log.write_text("다른 줄\n")
    assert soak_check.soak(clock.log, clock.log.stat().st_size, 3, probe=True) == []
    assert "미확인" in capsys.readouterr().out


def _answer(monkeypatch, code, body):
    def urlopen(request, timeout):
        assert request.get_header("X-deck-auth") == "x" and request.full_url == "http://192.0.2.7/usage"
        raise urllib.error.HTTPError(request.full_url, code, "x", {}, io.BytesIO(body))

    monkeypatch.setattr(soak_check.urllib.request, "urlopen", urlopen)


def test_send_bad_auth_accepts_only_401_bad(monkeypatch):
    _answer(monkeypatch, 401, b'{"auth":"bad"}')
    assert soak_check.send_bad_auth("192.0.2.7") == (True, None)
    _answer(monkeypatch, 401, b'{"auth":"stale"}')
    assert soak_check.send_bad_auth("192.0.2.7")[1]


def test_send_bad_auth_treats_no_answer_as_retry_and_success_as_failure(monkeypatch):
    def refused(request, timeout):
        raise urllib.error.URLError("refused")

    monkeypatch.setattr(soak_check.urllib.request, "urlopen", refused)
    assert soak_check.send_bad_auth("192.0.2.7")[0] is False
    monkeypatch.setattr(soak_check.urllib.request, "urlopen", lambda request, timeout: io.BytesIO(b"{}"))
    assert soak_check.send_bad_auth("192.0.2.7")[1]


def test_no_route_is_not_blamed_on_the_device(clock, monkeypatch, capsys):
    """Mac 에서 Python 에 로컬 네트워크 권한이 없으면 No route to host 로 끝난다. 거짓 실패로 보고하지 않는다."""
    def no_route(request, timeout):
        raise urllib.error.URLError(OSError(errno.EHOSTUNREACH, "No route to host"))

    monkeypatch.undo()
    monkeypatch.setattr(soak_check.time, "monotonic", lambda: clock.now)
    monkeypatch.setattr(soak_check.time, "sleep", clock.sleep)
    monkeypatch.setattr(soak_check.urllib.request, "urlopen", no_route)
    assert soak_check.soak(clock.log, clock.log.stat().st_size, 3, probe=True) == []
    assert "미확인" in capsys.readouterr().out


def test_cli_exit_code(clock, monkeypatch, capsys):
    clock.later = {1.0: "  기기> Guru Meditation Error: Core 1\n"}
    monkeypatch.setattr(sys, "argv", ["soak_check.py", "--log", str(clock.log), "--seconds", "3"])
    assert soak_check.main() == 1
    assert "Guru Meditation" in capsys.readouterr().out
    clock.later = {}
    assert soak_check.main() == 0
