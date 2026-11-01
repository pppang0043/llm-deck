"""LLM Deck PC 도우미.

    uv run main.py                  사용량을 30초마다 기기로 보내고 마이크 소리를 받는다
    uv run main.py --link usb       연결 방식을 고른다: usb / ble / wifi (기본: 되는 것을 자동 선택)
    uv run main.py --dry-run        기기로 보내지 않고, 보낼 내용만 출력한다
    uv run main.py --record a.wav   마이크 소리를 10초간 WAV 로 저장하고 끝낸다 (수신 확인용)
    uv run main.py --mic on         기기의 마이크를 PC 에서 켠다 (off 로 끈다)
    uv run main.py --battery 4.12   멀티미터로 잰 배터리 전압(V)을 알려 기기의 배터리 표시를 보정한다 (0: 보정 지움)
    uv run main.py --camera shoot   기기 카메라로 사진을 찍어 저장한다 (open / close 로 화면만 열고 닫기)
    uv run main.py --camera-mount 1 사진이 돌아가 보이면 카메라 센서 방향을 맞춘다 (0~3, 좌우 반전이면 1m 처럼)
    uv run main.py --test-video 5   기기 카메라로 5초 녹화해 MOV 로 저장한다
    uv run main.py --screenshot     기기 화면을 찍어 저장한다 (--view home 과 함께 쓰면 그 화면으로 바꾼 뒤)
    uv run main.py --mirror         켜져 있는 도우미의 미러링 창을 연다 (미러링 앱, 없으면 브라우저. 기기 화면을 보고 눌러서 조작, USB·Wi-Fi)
    uv run main.py --test-photo     카메라 없이 무늬 사진을 찍은 것처럼 해서 SD 카드·PC 저장을 시험한다
    uv run main.py --test-sound     기기 알림에 쓰는 소리(마이크 켬·끔, 한도 알림)를 모두 들어 본다
    uv run main.py --bt-check       기기가 블루투스 스피커에 붙을 수 있는지 시험한다 (칩 지원 범위 + 주변 기기 찾기, USB)
    uv run main.py --bt-check all   위 + 연결할 수 있는 기기마다 붙어서 BLE 서비스 목록을 읽는다 (주소를 주면 그 기기만)
    uv run main.py --assistant-check 기기 질문에 쓸 Claude CLI 를 찾는다 (claude-test 먼저, 없으면 claude)
    uv run main.py --add-wifi       기기가 접속할 Wi-Fi 를 추가로 등록한다 (USB 로 연결한 상태에서)
    uv run main.py --clear-wifi     추가로 등록한 Wi-Fi 를 모두 지운다
    uv run main.py --sync-only      기기 없이, 이 PC 의 한도 사용률을 다른 PC 와 공유만 한다
    uv run main.py --setup-sync     공유에 쓸 비공개 gist 를 만든다 (처음 한 번)
    uv run main.py --install-statusline    Claude Code 한도 사용률을 받아오도록 설정한다 (PC 마다 한 번)
"""

import argparse
import faulthandler
import getpass
import json
import os
import platform
import socket
import subprocess
import sys
import threading
import time
import tomllib
import traceback
from datetime import datetime
from pathlib import Path

# Windows 설치(windows/install.py)는 가상 환경 없이 이 PC 의 Python 으로 돌고, 부품은 이 폴더의 lib 에 풀어 둔다
_LIB = Path(__file__).resolve().parent / "lib"
if _LIB.is_dir() and str(_LIB) not in sys.path:
    sys.path.insert(1, str(_LIB))

import assistant  # noqa: E402 (위에서 부품 경로를 넣은 뒤에 불러온다)
import audio
import photos
import videos
import sounds
import launcher
import mirror
import orca
import pcstats
import statusline
import typer
import sync
import version
import crash_watch
from collectors import COLLECTORS, LIMITS
from collectors.fmt import limits_page
from collectors import orca_usage
from transports import TRANSPORTS, frames
from transports.commands import CommandTransport, CommandError

INTERVAL = 30
# 블루투스로 붙어 있을 때 Wi-Fi 를 다시 찾아보는 간격 (초)
WIFI_RETRY = 300
INSTANCE_PORT = 4012  # 도우미가 하나만 돌게 하려고 잡는 PC 안쪽 포트
RECORD_SECONDS = 10
# 기기와 주고받는 바퀴가 이만큼 돌아오지 않으면 멈춘 것으로 보고 도우미를 다시 띄운다.
# 한 바퀴는 보통 1초 안쪽이고, 인터넷이 느려 모든 요청이 한계시간(10~20초)을 채워도 몇 분이면 끝난다.
HANG_SECONDS = 600
# --log 기록 파일이 이보다 크면 시작할 때 .1 로 넘기고 새로 쓴다 (하나만 남긴다. Mac 은 앱이 같은 일을 한다)
LOG_LIMIT = 2_000_000


def load_config() -> dict:
    path = Path(__file__).parent / "config.toml"
    if not path.exists():
        return {}
    # Windows 메모장이 앞에 붙이는 BOM 은 떼고 읽는다
    text = path.read_bytes().decode("utf-8-sig")
    try:
        return tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        # 고친 설정에 틀린 곳이 있으면 어디인지 알려 주고 멈춘다 (Windows 경로의 \ 는 작은따옴표 '...' 로 적어야 한다)
        sys.exit(f"config.toml 을 읽지 못했습니다: {e}\n"
                 "Windows 경로는 작은따옴표로 적으세요 (예: model_dir = 'D:\\LLM Deck\\models')")


# 설정 섹션 이름 -> (마지막 호출 시각, 마지막 결과)
_last: dict[str, tuple[float, dict | None]] = {}


_sync: sync.Sync | None = None

# 설정 섹션 이름 -> 마지막 수집 오류 (성공하면 지운다). 기기 설정 > 정보 에 한 줄로 보여준다
_errors: dict[str, str] = {}


def _error_text(e: Exception) -> str:
    return str(e).strip() or type(e).__name__


def collect_limits(cfg: dict) -> list:
    """구독 한도 페이지들. 이 PC 에 값이 있으면 그것을 보여주고, 없을 때만 다른 PC 가 올려 둔 값을 쓴다."""
    global _sync
    if _sync is None:
        _sync = sync.Sync(cfg.get("sync", {}))
    enabled = [(section, module) for section, module in LIMITS if cfg.get(section, {}).get("enabled", True)]
    local = {}
    for section, module in enabled:
        try:
            local[section] = module.snapshot(cfg.get(section, {}))
            _errors.pop(section, None)
        except Exception as e:
            print(f"[{section}] 수집 실패: {e}")
            _errors[section] = _error_text(e)
    # 공유 저장소에는 여러 PC 의 값을 합친 것을 올린다 (화면에 무엇을 보여주는지와는 별개)
    shared = _sync.merge(local)
    pages = []
    for section, module in enabled:
        snapshot = local.get(section) or shared.get(section)
        if snapshot:
            pages.append(limits_page(module.NAME, snapshot))
        elif hasattr(module, "hint") and module.hint():
            pages.append(module.hint())
    # 기존 CLI/공유 페이지는 유지하고, 현재 PC의 ORCA Codex를 별도 페이지로 추가한다.
    if cfg.get("orca", {}).get("usage", True) and cfg.get("codex", {}).get("enabled", True):
        orca_data = orca_usage.read()
        if orca_data:
            page = orca_usage.pages(orca_data, {**cfg, "claude_code": {"enabled": False}}).get("codex")
            if page:
                pages.append(page)
    return pages


def collect_all(cfg: dict) -> dict:
    services = collect_limits(cfg)
    extra = {"sync_url": _sync.public_url} if _sync.public_url else {}
    for section, module, min_interval in COLLECTORS:
        called_at, result = _last.get(section, (0.0, None))
        if time.time() - called_at >= min_interval:
            try:
                result = module.collect(cfg)
                _errors.pop(section, None)
            except Exception as e:  # 한 서비스가 실패해도 나머지는 계속 보여준다
                print(f"[{section}] 수집 실패: {e}")
                _errors[section] = _error_text(e)
            _last[section] = (time.time(), result)
        if result:
            services.append(result)
    # 기기에는 시계가 없어서(Wi-Fi 가 없으면 시각을 모른다) 보낼 때마다 현재 시각을 알려준다
    clock = {"now": int(time.time()), "utc_offset": time.localtime().tm_gmtoff}
    # 기기의 블루투스 설정 화면에 여러 PC 를 이름으로 보여주려고 이 PC 이름을 함께 보낸다.
    # PC 종류는 단축키 버튼 이름을 맞추는 데 쓴다 (Mac 에서는 파워쉘 대신 터미널)
    os_name = "mac" if sys.platform == "darwin" else "win" if sys.platform == "win32" else "linux"
    return {"updated": datetime.now().strftime("%H:%M"), "services": services, "host": _host_name(), "os": os_name,
            **clock, **extra}


class Collector:
    """사용량 모으기(gist·API 요청, 느리면 수십 초)를 따로 돌린다.

    그동안에도 기기와 주고받는 바퀴(미러링 유지 신호, 나오는 중인 답, 알림)가 멈추지 않게 한다.
    한 번에 하나만 돌고, 결과는 잠금 아래에서 바꿔 끼운다.
    """

    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.lock = threading.Lock()
        self.thread: threading.Thread | None = None
        self.started_at = 0.0
        self.result: dict | None = None  # 마지막으로 모은 것
        self.fresh = False  # 아직 가져가지 않은 새 결과인지
        self.failed = False  # 마지막 모으기가 오류로 끝났는지
        self.collected_at = 0.0  # 마지막으로 다 모은 시각
        self.errors: dict[str, str] = {}  # 그때의 서비스별 오류 (기기 정보 화면에 보여준다)

    def start(self, limits_only: bool = False) -> None:
        """모으기를 시작한다. 앞의 모으기가 아직 돌고 있으면 그대로 둔다 (겹쳐 돌지 않는다)."""
        with self.lock:
            if self.thread and self.thread.is_alive():
                if time.monotonic() - self.started_at > HANG_SECONDS:
                    print(f"[collect] 사용량 모으기가 {HANG_SECONDS // 60}분 넘게 끝나지 않아 도우미를 다시 띄웁니다. 멈춘 곳:")
                    try:
                        faulthandler.dump_traceback(sys.stdout, all_threads=True)
                        sys.stdout.flush()
                    except (OSError, ValueError, AttributeError):
                        pass
                    restart()
                return
            self.started_at = time.monotonic()
            self.thread = threading.Thread(target=self._run, args=(limits_only,), daemon=True)
            self.thread.start()

    def _run(self, limits_only: bool) -> None:
        try:
            if limits_only:
                # 기기가 없을 때: 이 PC 의 한도 사용률 공유만 한다
                collect_limits(self.cfg)
                return
            result = collect_all(self.cfg)
        except Exception as e:
            print("[collect] 사용량을 모으지 못했습니다:")
            traceback.print_exc()
            with self.lock:
                self.failed = True
                self.errors = {"collect": _error_text(e), **_errors}
            return
        with self.lock:
            self.result, self.fresh, self.failed = result, True, False
            self.collected_at, self.errors = time.time(), dict(_errors)

    def take(self, wait: float) -> dict:
        """보낼 사용량. 새 결과를 wait 초까지 기다리고, 그래도 아직이면 지난 결과를 준다.

        지난 결과가 없으면(처음) 끝날 때까지 기다린다. 처음 모으기가 오류로 끝나면 오류를 낸다 (예전처럼 다시 시도한다).
        """
        deadline = time.monotonic() + wait
        while True:
            with self.lock:
                result, fresh, running = self.result, self.fresh, bool(self.thread and self.thread.is_alive())
                if result and (fresh or not running or time.monotonic() >= deadline):
                    self.fresh = False
                    # 받는 쪽이 고쳐 쓰므로 복사해 준다. 기기 시계는 보내는 시각으로 맞춘다
                    return {**result, "services": list(result["services"]), "now": int(time.time()),
                            "helper": version.status(self.collected_at, self.errors)}
                if not result and not running:
                    raise RuntimeError("사용량을 모으지 못했습니다" if self.failed else "사용량 모으기가 시작되지 않았습니다")
            time.sleep(0.1)


def _host_name() -> str:
    name = platform.node().removesuffix(".local") or "PC"
    # 기기 쪽 글꼴에 없는 글자나 따옴표가 섞이지 않게 영문·숫자·-_ 만 남긴다
    name = "".join(c for c in name if c.isascii() and (c.isalnum() or c in "-_ "))
    return name[:31] or "PC"


class Watchdog:
    """도우미가 어딘가에서 끝없이 기다리면(네트워크, 기기, 다른 프로그램) 스스로 다시 띄운다."""

    def __init__(self):
        self.beat_at = time.monotonic()
        threading.Thread(target=self._watch, daemon=True).start()

    def beat(self) -> None:
        self.beat_at = time.monotonic()

    def _watch(self) -> None:
        while True:
            time.sleep(30)
            stalled = time.monotonic() - self.beat_at
            if stalled < HANG_SECONDS:
                continue
            print(f"[watchdog] {stalled / 60:.0f}분 동안 멈춰 있어 도우미를 다시 띄웁니다. 멈춘 곳:")
            try:
                faulthandler.dump_traceback(sys.stdout, all_threads=True)
                sys.stdout.flush()
            except (OSError, ValueError, AttributeError):
                pass
            restart()


def restart() -> None:
    """같은 인자로 도우미를 새로 띄우고 이 도우미는 끝낸다."""
    if sys.platform == "win32":
        # Windows 의 자동 실행은 끝난 도우미를 다시 띄우지 않으므로 직접 띄운다. 새 도우미는 이 도우미가 끝나길 기다린다.
        env = dict(os.environ, LLMDECK_RESTARTED="1")
        subprocess.Popen([sys.executable, *sys.argv], env=env, cwd=Path(__file__).parent,
                         creationflags=subprocess.CREATE_NO_WINDOW)
        os._exit(1)
    # Mac, Linux: 같은 프로세스 자리에서 새로 시작한다 (Mac 앱 실행기는 그대로 기다린다)
    os.execv(sys.executable, [sys.executable, *sys.argv])


_instance_lock = None


def claim_device() -> bool:
    """기기에 붙는 도우미는 PC 에 하나만 돈다. 이미 돌고 있으면 False.

    USB 포트는 한 프로그램만 열 수 있고 같은 PC 에서 블루투스로 두 번 붙을 수도 없어서, 두 번째 도우미는
    기기를 "찾지 못했다"고만 나온다. 그 대신 무엇이 문제인지 알려 주려고 PC 안에서만 쓰는 포트를 잡는다.
    """
    global _instance_lock
    # 멈춰서 다시 띄운 도우미는 앞 도우미가 끝나 포트를 놓을 때까지 잠깐 기다린다
    tries = 20 if os.environ.pop("LLMDECK_RESTARTED", "") else 1
    for attempt in range(tries):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.bind(("127.0.0.1", INSTANCE_PORT))
            sock.listen(1)
            break
        except OSError:
            sock.close()
            if attempt == tries - 1:
                return False
            time.sleep(0.5)
    _instance_lock = sock
    return True


def usb_plugged(cfg: dict) -> bool:
    """기기가 USB 로 꽂혀 있는지 (포트가 보이는지만 본다)."""
    return TRANSPORTS["usb"](cfg.get("device", {}), lambda seq, pcm: None)._find_port() is not None


_usb_stuck = 0  # 기기에서 아무것도 받지 못한 채 USB 로 보내기가 연달아 실패한 횟수


def usb_health(transport, ok: bool) -> None:
    """PC 를 다시 켠 뒤 USB 포트는 보이는데 기기가 받지도 보내지도 않는 일이 있다. 그렇게 보이면 한 번 알린다.
    기기는 PC 에서 2분 동안 아무것도 오지 않으면 USB 를 다시 붙인다 (usb_link.cpp usb_reconnect)."""
    global _usb_stuck
    if ok or transport.name != "USB" or transport.last_rx > transport.opened_at:
        _usb_stuck = 0
        return
    _usb_stuck += 1
    if _usb_stuck == 3:
        print("[device] USB 포트는 보이는데 기기가 응답하지 않습니다. 기기가 2분 안에 USB 를 다시 붙입니다.")
        print("         그래도 안 되면 케이블을 뽑았다 꽂아 주세요.")


def connect(names: list[str], cfg: dict, on_audio, on_event, on_photo, on_video):
    for name in names:
        transport = TRANSPORTS[name](cfg.get("device", {}), on_audio, on_event, on_photo, on_video)
        if transport.open():
            print(f"[device] {transport.name} 로 연결했습니다.")
            return CommandTransport(transport)
        transport.close()
    print(f"[device] 기기를 찾지 못했습니다 ({', '.join(names)}). 다시 시도합니다.")
    return None


def wifi_setup(cfg: dict, add: bool) -> None:
    """기기에 Wi-Fi 를 등록하거나 지운다. 비밀번호가 무선으로 새지 않도록 USB 로만 보낸다."""
    if add:
        ssid = input("Wi-Fi 이름 (2.4GHz): ").strip()
        password = getpass.getpass("비밀번호 (입력해도 화면에 보이지 않습니다): ")
        if not ssid:
            return
        message = {"wifi_add": {"ssid": ssid, "password": password}}
    else:
        message = {"wifi_clear": True}
    transport = TRANSPORTS["usb"](cfg.get("device", {}), lambda seq, pcm: None)
    if not transport.open():
        print("[device] USB 로 연결된 기기를 찾지 못했습니다.")
        return
    transport = CommandTransport(transport)
    try:
        transport.send(message)
    finally:
        transport.close()
    print("[device] 등록했습니다. 최대 5개까지 기억합니다." if add else "[device] 지웠습니다. 기기를 다시 켜면 적용됩니다.")


def _private_files() -> None:
    """도우미 폴더의 기록에는 받아쓴 글이, 설정에는 키가 있다. 이 사용자만 읽게 둔다."""
    if sys.platform == "win32":
        return
    state = Path.home() / ".llm-deck"
    for path, mode in ((state, 0o700), (state / "companion.log", 0o600), (Path(__file__).parent / "config.toml", 0o600)):
        try:
            path.chmod(mode)
        except OSError:
            pass


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="LLM Deck PC companion")
    parser.add_argument("--link", choices=["auto", *TRANSPORTS], help="연결 방식 (기본: 설정 파일 값 또는 auto)")
    parser.add_argument("--dry-run", action="store_true", help="기기로 보내지 않고 보낼 내용만 출력")
    parser.add_argument("--record", metavar="WAV", help=f"마이크 소리를 {RECORD_SECONDS}초간 WAV 파일로 저장하고 종료")
    parser.add_argument("--mic", choices=["on", "off"], help="기기의 마이크를 켜거나 끈다")
    parser.add_argument("--install-statusline", action="store_true", help="Claude Code 한도 사용률을 받아오도록 설정")
    parser.add_argument("--uninstall-statusline", action="store_true", help="위 설정을 원래대로 되돌린다")
    parser.add_argument("--add-wifi", action="store_true", help="기기가 접속할 Wi-Fi 를 추가로 등록 (USB)")
    parser.add_argument("--clear-wifi", action="store_true", help="추가로 등록한 Wi-Fi 를 모두 지운다 (USB)")
    parser.add_argument("--command-status", action="store_true", help="마지막 미확정 명령의 기기 처리 상태 조회")
    parser.add_argument("--acknowledge-command", action="store_true", help="미확정 명령의 실제 결과를 확인한 뒤 PC 대기 기록만 해제 (재실행하지 않음)")
    parser.add_argument("--battery", type=float, metavar="V", help="멀티미터로 잰 배터리 전압(V)으로 기기를 보정한다")
    parser.add_argument("--camera", choices=["open", "shoot", "record", "close", "dump", "gallery", "scan"], help="기기의 카메라 화면을 열기 / 찍기 / 닫기 / 갤러리 / QR·바코드 연속 스캔")
    parser.add_argument("--camera-mount", choices=["0", "1", "2", "3", "0m", "1m", "2m", "3m"],
                        help="사진이 돌아가 보일 때: 센서 방향(시계 방향 90도 횟수), 좌우가 뒤집히면 m 을 붙인다")
    parser.add_argument("--camera-ctl", metavar="sharp=2,special=2",
                        help="카메라 화질 조절 (이름은 firmware/src/camera.cpp 의 CTLS, reset=1 이면 모두 기본값). 기기에 저장된다")
    parser.add_argument("--test-video", type=int, metavar="초", help="기기 카메라로 정한 초만큼 녹화해 저장한다")
    parser.add_argument("--links", metavar="usb=on,ble=off", help="기기의 연결 방식 켜고 끄기 (USB 로 연결했을 때만)")
    parser.add_argument("--view", metavar="화면",
                        help="기기 화면 바꾸기: home / usage / usage_N(사용량 N 번째 페이지, 0 부터) / info / settings, 설정 분류는 settings_wifi / settings_bluetooth / settings_speaker / "
                             "settings_notify / settings_display / settings_battery / settings_camera / settings_sd / "
                             "settings_general (settings_end 는 지금 화면의 맨 아래)")
    parser.add_argument("--sd-pull", action="store_true", help="기기 SD 카드의 마지막 동영상을 받아 온다 (USB, 진단용)")
    parser.add_argument("--sd-clear", action="store_true", help="기기 SD 카드의 사진을 모두 지운다 (USB)")
    parser.add_argument("--rotate", action="store_true", help="기기 화면을 90도 돌린다 (돌리기 버튼과 같다)")
    parser.add_argument("--saver", action="store_true", help="기기의 화면 보호기를 지금 띄운다 (화면을 만지면 사라짐)")
    parser.add_argument("--screenshot", action="store_true", help="기기 화면을 찍어 사진 폴더에 저장한다 (진단용)")
    parser.add_argument("--mirror", action="store_true", help="켜져 있는 도우미의 미러링 창을 연다 (미러링 앱, 없으면 브라우저)")
    parser.add_argument("--test-photo", action="store_true", help="카메라 없이 무늬 사진으로 사진 저장·전송을 시험한다")
    parser.add_argument("--test-sound", action="store_true", help="기기 알림에 쓰는 소리를 모두 들어 본다")
    parser.add_argument("--bt-check", nargs="?", const=True, metavar="주소|all",
                        help="기기가 블루투스 스피커에 붙을 수 있는지 시험한다 (USB). 주소나 all 을 주면 서비스 목록도 읽는다")
    parser.add_argument("--assistant-check", action="store_true", help="기기 질문에 쓸 Claude CLI 를 찾는다")
    parser.add_argument("--sync-only", action="store_true", help="기기 없이 한도 사용률 공유만 한다")
    parser.add_argument("--setup-sync", action="store_true", help="공유에 쓸 비공개 gist 를 만든다")
    parser.add_argument("--log", metavar="FILE", help="출력을 파일에 덧붙여 쓴다 (창 없이 자동 실행할 때)")
    return parser


def _log_to_file(name: str) -> None:
    path = Path(name).expanduser()
    try:
        if path.stat().st_size > LOG_LIMIT:
            path.replace(path.with_name(path.name + ".1"))
    except OSError:
        pass  # 없거나, 앞 도우미가 아직 잡고 있으면 (Windows) 이번에는 그대로 이어 쓴다
    sys.stdout = sys.stderr = open(path, "a", encoding="utf-8", buffering=1)


def _pairs(text: str) -> list[list[str]]:
    """"a=1,b=2" -> [["a", "1"], ["b", "2"]]"""
    return [item.split("=") for item in text.split(",")]


def _one_shot_fields(args) -> dict:
    """명령줄로 준 한 번짜리 명령. 첫 사용량 메시지에 함께 싣는다."""
    fields = {}
    if args.mic:
        fields["mic"] = args.mic == "on"
    if args.test_photo:
        fields["photo_test"] = True
    if args.camera:
        fields["camera"] = args.camera
    if args.links:
        fields["links"] = {k: v == "on" for k, v in _pairs(args.links)}
    if args.view:
        fields["view"] = args.view
    if args.screenshot:
        fields["screenshot"] = True
    if args.sd_pull:
        fields["sd_pull"] = True
    if args.sd_clear:
        fields["sd_clear"] = True
    if args.bt_check:
        fields["bt_check"] = args.bt_check
    if args.rotate:
        fields["rotate"] = True
    if args.saver:
        fields["saver"] = True
    if args.test_video:
        fields["camera"] = "record"
    if args.camera_mount:
        fields["camera_mount"] = {"rot": int(args.camera_mount[0]), "mirror": args.camera_mount.endswith("m")}
    if args.camera_ctl:
        fields["camera_ctl"] = {k: int(v) for k, v in _pairs(args.camera_ctl)}
    if args.battery is not None:
        fields["battery_mv"] = round(args.battery * 1000)
    return fields


class DeviceLoop:
    """기기와 주고받는 바퀴. 사용량을 INTERVAL 초마다 보내고, 그 사이에 작은 메시지를 보낸다.

    연결이 끊기면 다시 붙고, 예상하지 못한 오류가 나도 멈추지 않는다. 한 번짜리 명령이면 결과를 기다려 끝난다.
    """

    def __init__(self, args, cfg: dict, link: str):
        self.args = args
        self.cfg = cfg
        self.link = link
        self.names = list(TRANSPORTS) if link == "auto" else [link]
        self.sink = audio.Recorder(args.record) if args.record else audio.Player(cfg.get("audio", {}))
        self.speaker = sounds.Sounds(cfg.get("sounds", {}))
        self.movies = videos.VideoReceiver(cfg.get("videos", {}))
        self.receiver = photos.PhotoReceiver(cfg.get("photos", {}), self.movies.dir, cfg.get("scan", {}))
        self.apps = launcher.Launcher()
        self.pc = pcstats.PcStats()
        typer.configure(cfg.get("keywords"))  # config.toml 의 키워드 버튼 (없으면 기본 목록)
        self.keys = typer.Typer()
        self.ask = assistant.Assistant(cfg.get("assistant", {}), cfg.get("storage", {}).get("model_dir", ""))
        # ORCA 세션 상태 (10초마다), 기기에서 고른 세션에 받아쓰기
        self.sessions = orca.Orca(cfg.get("orca", {}))
        self.ask.orca = self.sessions
        # 기기 펌웨어와 버전이 맞는지 (기기가 {"fw_proto"} 로 알려 준다)
        self.peer = version.Peer()
        print(f"[version] 도우미 {version.version()} (proto {version.PROTO})")
        self.crash = crash_watch.CrashWatch()
        frames.log_sink = self.crash.on_log_line
        self.transport = None
        # 기기 화면 미러링 (브라우저 창). 화면 조각은 연결 쪽에서 바로 넘긴다
        self.view = mirror.Mirror(cfg.get("mirror", {}), lambda: self.transport)
        frames.mirror_sink = self.view.on_frame
        self.first = True  # 아직 첫 사용량 메시지를 보내지 못했다
        self.sent_mic = True
        self.sent_mic_status = None
        self.wifi_tried = time.time()
        self.collector = Collector(cfg)
        self.watchdog = Watchdog()

    def on_audio(self, seq: int, pcm: bytes) -> None:
        # 기기 마이크 소리: 가상 마이크로 내보내고, 질문 녹음 중이면 녹음에도 쓴다
        self.sink.push(seq, pcm)
        self.ask.on_device_audio(seq, pcm)

    def on_event(self, event: dict) -> None:
        if isinstance(event, dict) and event.get("link_error") == "not_queued":
            print("[device] 기기가 요청을 받지 못했습니다 (큐/메모리 제한 또는 잘못된 메시지). 잠시 뒤 다시 시도하세요.")
        if isinstance(event, dict) and event.get("link_error") == "not_owner":
            print("[device] 다른 PC 가 기기를 사용 중이라 제어 명령을 거절했습니다.")
        # 한 곳에서 오류가 나도 나머지는 알림을 받는다 (오류는 기록만 하고 받는 스레드는 계속 돈다)
        for handler in (self.speaker, self.receiver, self.movies, self.apps, self.keys, self.pc, self.ask,
                        self.sessions, self.peer, self.crash):
            try:
                handler.on_event(event)
            except Exception:
                frames.report(type(handler).__name__)

    def _handlers(self) -> tuple:
        return self.on_audio, self.on_event, self.receiver.on_chunk, self.movies.on_chunk

    def _connect(self) -> None:
        # 끊긴 연결로 받던 녹화가 있으면 받은 데까지 재생되는 파일로 마무리한다
        self.movies.finish()
        self.view.link_lost()
        self.transport = connect(self.names, self.cfg, *self._handlers())
        if not self.transport:
            return
        self.sessions.resend()  # 새로 연결한 기기에도 세션 목록을 보낸다
        self.peer.reset()
        if getattr(self.transport, "confirmed", True):
            self.view.link_up(self.transport.name)
        else:
            # USB 는 LLM Deck 인지 확인된 뒤에 알린다 (다른 ESP32 보드에 미러링 창을 열지 않게)
            self.transport.on_confirm = lambda name=self.transport.name: self.view.link_up(name)
            if self.transport.confirmed:  # 그 사이에 확인됐으면
                self.transport.take_confirm()

    def _send(self, message: dict) -> bool:
        """보내고 True. 연결 문제면 닫아서 다음 바퀴가 다시 붙게 하고 False. 명령 오류는 그대로 올린다."""
        try:
            self.transport.send(message)
            return True
        except CommandError:
            raise
        except OSError as e:
            print(f"[device] 전송 실패: {e}")
            usb_health(self.transport, False)
            self._disconnect()
            return False

    def _disconnect(self) -> None:
        self.transport.close()
        self.transport = None

    def _send_usage(self) -> None:
        # 모으기는 따로 돈다. 보통 금방 끝나니 잠깐 기다려 새 값을 보내고, 느리면 지난 값을 보낸다 (새 값은 다음 차례에)
        self.collector.start()
        message = self.collector.take(wait=2)
        # PC 마이크를 쓸 수 있는지 (없으면 기기의 말하기 버튼이 꺼진다)
        self.sent_mic = message["ask_mic"] = self.ask.mic
        # 기기 상태줄의 PC 마이크 표시와 마이크 고르기 창
        self.sent_mic_status = message["pc_mic"] = self.ask.mic_status()
        # 키워드 버튼 이름 (기기가 바뀐 것만 저장하고 단축키 화면·ORCA 팝업을 다시 만든다)
        message["keywords"] = typer.ITEMS
        # 기기 버전을 아직 모르면 다시 알려 달라고 한다 (도우미만 다시 켜면 기기는 같은 연결로 보고 알려 주지 않는다)
        if self.peer.proto is None:
            message["ask_fw"] = True
        if self.first:
            message.update(_one_shot_fields(self.args))
        if self._send(message):
            usb_health(self.transport, True)
            print(f"[device] {message['updated']} 사용량 전송 ({len(message['services'])}개 서비스)")
            self.first = False
            self.peer.sent()

    def _switch_to_faster_link(self) -> bool:
        """더 빠른 연결로 갈아탔으면 True. 기기도 같은 순서로 우선한다 (link.cpp)."""
        if not self.transport or self.link != "auto":
            return False
        # 블루투스·Wi-Fi 로 붙어 있는데 USB 케이블이 꽂히면 USB 로 갈아탄다
        if self.transport.name != "USB" and usb_plugged(self.cfg):
            print(f"[device] USB 케이블이 연결되어 {self.transport.name} 에서 USB 로 바꿉니다.")
            self._disconnect()
            return True
        # 블루투스로 붙어 있으면 가끔 Wi-Fi 를 찾아, 되면 갈아탄다 (더 빠르고 인증도 된다)
        if self.transport.name != "Bluetooth" or time.time() - self.wifi_tried <= WIFI_RETRY:
            return False
        self.wifi_tried = time.time()
        faster = TRANSPORTS["wifi"](self.cfg.get("device", {}), *self._handlers())
        if not faster.open():
            faster.close()
            return False
        print("[device] Wi-Fi 로 기기를 찾아 블루투스에서 Wi-Fi 로 바꿉니다.")
        self.transport.close()
        self.transport = CommandTransport(faster)
        self.sessions.resend()
        self.peer.reset()
        self.view.link_up(self.transport.name)
        return True

    def _wait_for_one_shot(self) -> bool:
        """한 번짜리 명령이 있었으면 결과를 기다리고 True. 계속 도는 명령(--mic 등)이면 False."""
        args = self.args
        if args.test_video:
            # 정해진 초만큼 녹화하고 멈춘 뒤, 파일이 다 쓰일 때까지 기다린다
            time.sleep(args.test_video)
            self.transport.send({"camera": "record"})
            for _ in range(30):
                time.sleep(1)
                if self.movies.writer is None:
                    break
            self.transport.send({"camera": "close"})
            time.sleep(1)
        elif args.camera in ("open", "close", "gallery") or args.camera_mount or args.camera_ctl or args.links:
            time.sleep(2)
        elif (args.view or args.saver or args.sd_clear or args.rotate) and not args.screenshot:
            time.sleep(1)
        elif args.test_photo or args.screenshot or args.sd_pull or args.camera in ("shoot", "dump"):
            # 사진이 다 올 때까지 기다린다 (블루투스는 수 초)
            for _ in range(60):
                time.sleep(1)
                if self.receiver.buf is None and self.receiver.name:
                    break
        elif args.bt_check:
            # 기기가 10초 동안 주변을 찾고 결과를 적는다 (USB 로그에 "기기> [bt-check]" 로 보인다)
            # 서비스 목록까지 읽으면 기기마다 수 초씩 더 걸린다 (최대 8대)
            time.sleep(20 if args.bt_check is True else 120)
            print("[bt-check] 블루투스 스피커는 보통 클래식 블루투스(A2DP)로 소리를 받습니다.")
            print("           위에 'Bluetooth Classic ... NO' 이면 이 기기의 칩으로는 그런 스피커에 소리를 보낼 수 없습니다.")
            print("           [LE Audio] 표시가 있는 스피커가 있어도, ESP32-S3 는 LE Audio(BLE 5.2) 도 지원하지 않습니다.")
        elif args.battery is not None:
            print(f"[device] 배터리 전압 {args.battery:.3f}V 로 보정했습니다." if args.battery else "[device] 배터리 보정을 지웠습니다.")
            time.sleep(1)
        else:
            return False
        return True

    def _finished(self) -> bool:
        """한 번짜리 명령을 마쳤으면 연결을 닫고 True."""
        if self.transport and not self.first and self._wait_for_one_shot():
            self.transport.close()
            return True
        if not self.args.record:
            return False
        if self.transport:
            print(f"[audio] {RECORD_SECONDS}초간 녹음합니다. 기기의 마이크가 켜져 있어야 합니다.")
            time.sleep(RECORD_SECONDS)
            self.transport.close()
        self.sink.close()
        return True

    def _small_updates(self) -> dict:
        """기기 화면이 원할 때만 보내는 작은 메시지: PC 상태, 단축 버튼 결과 알림, 말로 묻기 상태 등."""
        extra = {}
        if self.pc.due():
            extra["pc"] = self.pc.collect()
        for source in (self.apps, self.keys, self.ask, self.sessions, self.receiver):
            if source.pending_toast:
                # 메시지 하나에 알림 하나만 실린다. 나머지는 다음 차례에 보낸다
                extra["toast"], source.pending_toast = source.pending_toast, None
                break
        if self.receiver.pending_scan:  # 기기 카메라의 연속 스캔 결과 (photos.py)
            extra["scan"], self.receiver.pending_scan = self.receiver.pending_scan, None
        if self.receiver.pending_roi is not None:  # 스캔 1단계 장의 바코드 자리 ([] 면 못 찾음)
            extra["scan_roi"], self.receiver.pending_roi = self.receiver.pending_roi, None
        # 말로 묻기: 상태(듣는 중·생각 중)와 나오는 중인 답
        orca_state = self.sessions.pending()
        if orca_state:
            extra["orca"] = orca_state
        answer = self.ask.pending()
        if answer:
            extra["ask"] = answer
        if self.ask.mic != self.sent_mic:
            extra["ask_mic"] = self.sent_mic = self.ask.mic
        if self.ask.mic_status() != self.sent_mic_status:
            extra["pc_mic"] = self.sent_mic_status = self.ask.mic_status()
        extra.update(self.view.pending() or {})
        return extra

    def _serve_until_next_usage(self) -> None:
        # 다음 전송까지 기다리되, 연결이 끊기면(기기를 껐다 켬, 블루투스 끊김) 바로 다시 연결한다
        for _ in range(INTERVAL * 2):
            self.watchdog.beat()
            if not self.transport.connected():
                print(f"[device] {self.transport.name} 연결이 끊겼습니다. 다시 연결합니다.")
                self._disconnect()
                return
            extra = self._small_updates()
            if extra and not self._send({**extra, "host": _host_name()}):
                return
            time.sleep(0.5)

    def _tick(self) -> bool:
        """한 바퀴. 한 번짜리 명령을 마쳐 끝낼 때만 True."""
        if self.transport is None:
            self._connect()
        if self.transport:
            self._send_usage()
            if self._switch_to_faster_link():
                return False
        else:
            # 기기가 없어도 이 PC 의 한도 사용률은 계속 공유한다 (기기가 클라우드로 읽어 갈 수 있게)
            self.collector.start(limits_only=True)
        if self._finished():
            return True
        if self.transport:
            self._serve_until_next_usage()
        else:
            time.sleep(2)
        return False

    def run(self) -> None:
        while True:
            self.watchdog.beat()
            try:
                if self._tick():
                    return
            except CommandError as exc:
                if self.transport:
                    self.transport.close()
                # 명령이 실행됐을 수 있으므로 first=True 인 채 재연결해 새 ID 로 반복하지 않는다.
                raise SystemExit(f"[device] {exc}") from exc
            except Exception:
                # 예상하지 못한 오류로 도우미가 꺼지지 않게 한다. Windows 의 자동 시작은 꺼지면 다음 로그인까지 다시 켜지지 않는다.
                print("[device] 오류가 나서 10초 뒤 다시 시작합니다:")
                traceback.print_exc()
                if self.transport:
                    try:
                        self.transport.close()
                    except Exception:
                        pass
                self.transport = None
                time.sleep(10)


def _run_without_device(args, cfg: dict) -> bool:
    """기기 바퀴 없이 끝나는 명령을 처리했으면 True."""
    if args.assistant_check:
        found = assistant.find_command(cfg.get("assistant", {}))
        print(f"[assistant] 질문 폴더: {assistant.workdir()}")
        sys.exit(0 if found else 1)
    if args.test_sound:
        speaker = sounds.Sounds(cfg.get("sounds", {}))
        for name in sounds.TONES:
            print(f"[sound] {name}")
            speaker.play(name)
            time.sleep(1)
        return True
    if args.add_wifi or args.clear_wifi:
        if not claim_device():
            raise SystemExit("이미 실행 중인 PC 도우미를 먼저 종료하세요.")
        wifi_setup(cfg, add=args.add_wifi)
        return True
    if args.setup_sync:
        sync.create(cfg.get("sync", {}))
        return True
    if args.sync_only:
        print("[sync] 이 PC 의 한도 사용률을 공유합니다. (Ctrl+C 로 종료)")
        while True:
            collect_limits(cfg)
            time.sleep(INTERVAL)
    if args.mirror:
        if not mirror.open_window():
            sys.exit(1)  # mirror.cmd 가 창을 닫지 않고 이유를 보여 주게
        return True
    if args.dry_run:
        print(json.dumps(collect_all(cfg), indent=2, ensure_ascii=False))
        return True
    return False


def _print_command_status(args, names: list[str], cfg: dict) -> None:
    transport = connect(names, cfg, lambda *a: None, lambda *a: None, lambda *a: None, lambda *a: None)
    if not transport:
        raise SystemExit(1)
    try:
        print(json.dumps(transport.command_status(acknowledge=args.acknowledge_command), ensure_ascii=False))
    finally:
        transport.close()


def main() -> None:
    args = _parser().parse_args()
    _private_files()
    if hasattr(faulthandler, "register"):
        import signal
        # 멈춘 것 같을 때 kill -USR1 <pid> 로 어디서 기다리는지 기록에 남긴다 (Mac, Linux)
        faulthandler.register(signal.SIGUSR1, all_threads=True)
    if args.log:
        _log_to_file(args.log)
    if args.install_statusline:
        return statusline.install()
    if args.uninstall_statusline:
        return statusline.uninstall()
    cfg = load_config()
    if _run_without_device(args, cfg):
        return
    if not claim_device():
        log = Path.home() / ".llm-deck" / "companion.log"
        print("[device] 이 PC 에서 이미 다른 도우미가 기기에 연결되어 있습니다 (로그인할 때 자동으로 켜진 것일 수 있습니다).")
        print(f"         그 도우미의 기록: {log}")
        print("         여기서 직접 실행하려면 먼저 그 도우미를 끄세요.")
        print("         Windows: 작업 관리자에서 python.exe 종료 / Mac: launchctl bootout gui/$(id -u)/com.llmdeck.companion")
        return
    link = args.link or cfg.get("device", {}).get("link", "auto")
    if args.command_status or args.acknowledge_command:
        return _print_command_status(args, list(TRANSPORTS) if link == "auto" else [link], cfg)
    DeviceLoop(args, cfg, link).run()


if __name__ == "__main__":
    main()
