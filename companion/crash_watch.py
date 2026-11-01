"""기기가 오류로 재시작한 것을 알아채고 알린다.

2026-11-01 에 기기가 1분마다 재시작했는데 며칠 동안 아무도 몰랐다 (기록 파일에만 남았다). 두 곳에서 알아낸다:
    USB 로그 줄  Guru Meditation, assert failed 등. 기기가 죽기 직전에 찍는다
    기기 알림    {"crash": {"reason": "panic", "count": N, "uptime_s": U, "build": "..."}}. 다시 켜진 뒤 보낸다 (Wi-Fi 는 이것뿐)
USB 로는 둘 다 오므로 한 번으로 센다. firmware/scripts/soak_check.py 도 이 파일의 is_crash_line 을 쓰므로
표준 라이브러리만 불러온다.
"""

import re
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from collections import deque
from pathlib import Path

_CRASH_LINE = re.compile(r"Guru Meditation|assert failed|abort\(\)|Task watchdog|^Rebooting\.\.\.$")
# 한 번 죽을 때 찍는 줄들은 한 번으로 친다. Rebooting... 이 나오면 그 전에 끝난다 (빠른 부트 루프도 따로 세게)
EPISODE_SECONDS = 10
# 죽은 뒤 다시 켜져 알림을 보내기까지 걸리는 시간. 이 안의 기기 알림은 USB 로그로 이미 센 것이다
EVENT_MERGE_SECONDS = 90
HOUR = 3600
LOOP_COUNT = 3  # 이 횟수를
LOOP_SECONDS = 600  # 이 시간 안에 넘으면 크래시 루프
NOTIFY_GAP = 600  # 바탕화면 알림 간격. 기록과 횟수는 매번 남긴다
TELEGRAM_GAP = 3600
TELEGRAM_TIMEOUT = 10
TELEGRAM_PREFIX = "[IOT-DECK S3]"  # 공용 봇이라 메시지 앞에 서비스 이름을 붙인다


def is_crash_line(text: str) -> bool:
    """기기 로그 한 줄이 죽을 때 찍히는 줄인지. [link] 같은 태그 줄은 받은 글자를 그대로 찍을 수 있어 보지 않는다."""
    return not text.startswith("[") and bool(_CRASH_LINE.search(text))


def desktop_notify(text: str) -> None:
    """Mac/Windows 알림. 받는 스레드에서 부르지 않는다 (_background)."""
    try:
        if sys.platform == "darwin":
            # 글자를 스크립트에 끼워 넣지 않고 인자로 넘긴다
            subprocess.run(["osascript", "-e", "on run argv", "-e", 'display notification (item 1 of argv) with title "LLM Deck"',
                            "-e", "end run", "--", text], capture_output=True, timeout=10, check=True)
        elif sys.platform == "win32":
            # 트레이 앱(app.pyw)과 같은 pystray 알림. 도우미는 트레이 앱과 다른 프로세스라 아이콘을 잠깐 따로 띄운다
            import pystray
            from PIL import Image

            icon = pystray.Icon("LLM Deck 기기 오류", Image.new("RGB", (16, 16), "red"), "LLM Deck")
            icon.run_detached()
            time.sleep(1)
            icon.notify(text, "LLM Deck")
            time.sleep(10)
            icon.stop()
    except Exception as e:
        print(f"[crash] 바탕화면 알림을 띄우지 못했습니다: {type(e).__name__}")


def _telegram_env() -> dict[str, str]:
    env = {}
    try:
        lines = (Path.home() / ".secrets" / "telegram.env").read_text().splitlines()
    except (OSError, ValueError):
        return env
    for line in lines:
        name, sep, value = line.strip().removeprefix("export ").partition("=")
        if sep:
            env[name.strip()] = value.strip().strip("'\"")
    return env


def send_telegram(text: str) -> None:
    """공용 봇으로 보낸다. 파일이 없으면 조용히 넘어간다. 오류에도 주소(토큰 포함)를 적지 않게 종류만 남긴다."""
    env = _telegram_env()
    token, chat = env.get("TELEGRAM_BOT_TOKEN"), env.get("TELEGRAM_CHAT_ID")
    if not token or not chat:
        return
    body = urllib.parse.urlencode({"chat_id": chat, "text": f"{TELEGRAM_PREFIX} {text}"}).encode()
    try:
        urllib.request.urlopen(f"https://api.telegram.org/bot{token}/sendMessage", data=body, timeout=TELEGRAM_TIMEOUT).close()
    except Exception as e:
        print(f"[crash] 텔레그램을 보내지 못했습니다: {type(e).__name__}")


def _background(target, *args) -> None:
    threading.Thread(target=target, args=args, daemon=True).start()


class CrashWatch:
    def __init__(self):
        self._lock = threading.Lock()  # USB·Wi-Fi·블루투스 받는 스레드가 따로 부른다
        self._crashes = deque()  # 최근 1시간의 시각
        self._episode_until = 0.0  # 이 시각 전의 로그 줄은 같은 죽음의 줄이다
        self._log_crash_at = None  # USB 로그로 세고 아직 기기 알림과 짝짓지 않은 시각
        self._last_event = None
        self._last_count = 0
        self._notified_at = None
        self._telegram_at = None

    def on_log_line(self, text: str) -> None:
        if not is_crash_line(text):
            return
        with self._lock:
            now = time.monotonic()
            same = now < self._episode_until
            # Rebooting... 뒤의 줄은 다음 죽음이다
            self._episode_until = 0.0 if text == "Rebooting..." else now + EPISODE_SECONDS
            if not same:
                self._log_crash_at = now
                self._count(now, f"USB 로그: {text[:100]}")

    def on_event(self, event) -> None:
        crash = event.get("crash") if isinstance(event, dict) else None
        if not isinstance(crash, dict):
            return
        count = crash.get("count")
        key = (crash.get("build"), count, crash.get("uptime_s"))
        with self._lock:
            # 같은 알림을 또 받아도 한 번이다. count 는 연속 횟수라 오래 살아남은 뒤에는 다시 1 이므로 uptime 까지 본다
            if count is not None and key == self._last_event:
                return
            self._last_event = key
            # 알리지 못하고 죽은 부팅이 있으면 다음 알림의 count 가 그만큼 뛴다 (firmware/src/crash_log.h)
            new = 1
            if isinstance(count, int) and count > 0:
                new = count - self._last_count if count > self._last_count else count
                self._last_count = count
            now = time.monotonic()
            if self._log_crash_at is not None and now - self._log_crash_at < EVENT_MERGE_SECONDS:
                self._log_crash_at = None
                return
            self._count(now, f"기기 알림: {str(crash.get('reason'))[:40]}, 연속 {count}번, "
                             f"가동 {crash.get('uptime_s')}초, build {str(crash.get('build'))[:20]}", new)

    def _count(self, now: float, detail: str, times: int = 1) -> None:
        self._crashes.extend([now] * times)
        while now - self._crashes[0] >= HOUR:
            self._crashes.popleft()
        hour = len(self._crashes)
        print(f"[crash] 기기가 오류로 재시작했습니다 (최근 1시간 {hour}번). {detail}", flush=True)
        if self._notified_at is None or now - self._notified_at >= NOTIFY_GAP:
            self._notified_at = now
            _background(desktop_notify, f"기기가 오류로 재시작했습니다 (최근 1시간 {hour}번)")
        recent = sum(now - at < LOOP_SECONDS for at in self._crashes)
        if recent >= LOOP_COUNT and (self._telegram_at is None or now - self._telegram_at >= TELEGRAM_GAP):
            self._telegram_at = now
            print(f"[crash] 크래시 루프입니다 ({LOOP_SECONDS // 60}분에 {recent}번).", flush=True)
            _background(send_telegram, f"크래시 루프: {LOOP_SECONDS // 60}분에 {recent}번 재시작. {detail}")
