"""펌웨어를 올린 뒤 도우미 기록을 지켜봐 기기가 죽지 않는지 확인한다. 문제가 있으면 그 줄을 보여 주고 1 로 끝난다.

2026-11-01 에 거절 경로의 버그로 기기가 1분마다 재시작했는데, 올리기 검증은 부팅 직후만 봐서 통과했다.
--probe 는 그 경로를 일부러 지나가게 잘못된 인증 요청을 보낸다. 형식은 맞고 nonce 만 모르는 머리글은 stale 로
끝나 거절 경로까지 가지 않으므로 형식부터 틀린 값을 보낸다.
"""
import argparse
import errno
import importlib.util
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_LOG = Path.home() / ".llm-deck/companion.log"
DEFAULT_SECONDS = 120
PROBE_GAP = 5  # 기기가 아직 Wi-Fi 에 붙지 않았으면 이 간격으로 다시 보낸다
DEVICE_PREFIX = "기기> "
_IP = re.compile(r"기기> \[net\] connected, ip (\d+\.\d+\.\d+\.\d+)")

# 판정을 도우미와 같게 한다. companion/ 의 다른 파일(typer, version...)을 가리지 않게 이 파일만 불러온다
_spec = importlib.util.spec_from_file_location(
    "llmdeck_crash_watch", Path(__file__).resolve().parents[2] / "companion/crash_watch.py")
_crash_watch = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_crash_watch)


def log_size(log: Path) -> int:
    try:
        return log.stat().st_size
    except OSError:
        return 0


def _is_crash(line: str) -> bool:
    text = line.strip()
    if text.startswith("[crash] "):
        return True
    return text.startswith(DEVICE_PREFIX) and _crash_watch.is_crash_line(text[len(DEVICE_PREFIX):])


def send_bad_auth(ip: str) -> tuple[bool, str | None]:
    """형식이 틀린 인증 요청을 보낸다. (대답을 받았는지, 문제). 대답이 없으면 (False, 이유) 로 다시 시도한다."""
    request = urllib.request.Request(f"http://{ip}/usage", data=b"{}", method="POST",
                                     headers={"Content-Type": "application/json", "X-Deck-Auth": "x"})
    try:
        urllib.request.urlopen(request, timeout=5).close()
    except urllib.error.HTTPError as e:
        try:
            reason = json.loads(e.read()).get("auth")
        except (ValueError, AttributeError, OSError):
            reason = None
        if e.code == 401 and reason == "bad":
            return True, None
        return True, f"잘못된 인증 요청에 HTTP {e.code} ({reason}) 로 답했습니다 ({ip}). 401 bad 여야 합니다"
    except (urllib.error.URLError, OSError) as e:
        # Mac 은 이 Python 에 "로컬 네트워크" 권한이 없어도 이렇게 끝난다 (README). 기기 문제라고 할 수 없다
        if getattr(getattr(e, "reason", e), "errno", None) == errno.EHOSTUNREACH:
            return False, None
        return False, f"잘못된 인증 요청에 대답이 없습니다 ({ip}): {e}"
    return True, f"잘못된 인증 요청이 통과했습니다 ({ip})"


def soak(log: Path, offset: int, seconds: float = DEFAULT_SECONDS, build: str | None = None, probe: bool = False) -> list[str]:
    """offset 뒤의 기록을 seconds 동안 본다. 문제 목록을 돌려준다 (비었으면 통과)."""
    deadline = time.monotonic() + seconds
    ip = None
    if probe:
        # 기기는 기록이 다시 이어지기 전에 이미 Wi-Fi 에 붙었을 수 있으므로 앞부분에서도 찾는다
        try:
            found = _IP.findall(log.read_bytes()[:offset].decode(errors="replace"))
            ip = found[-1] if found else None
        except OSError:
            pass
    probe_at = 0.0
    probe_error = None
    build_seen = build is None
    partial = b""
    while True:
        size = log_size(log)
        if size < offset:  # 도우미가 켜지면서 기록을 .1 로 넘겼다
            offset, partial = 0, b""
        if size > offset:
            with log.open("rb") as f:
                f.seek(offset)
                data = f.read()
            offset += len(data)
            *lines, partial = (partial + data).split(b"\n")
            for raw in lines:
                line = raw.decode(errors="replace")
                if _is_crash(line):
                    return [f"크래시 줄: {line.strip()}"]
                build_seen = build_seen or build in line
                if probe:
                    match = _IP.search(line)
                    ip = match.group(1) if match else ip
        now = time.monotonic()
        if probe and ip and now >= probe_at:
            answered, probe_error = send_bad_auth(ip)
            probe_at = now + PROBE_GAP
            if answered:
                if probe_error:
                    return [probe_error]
                print(f"[soak] 잘못된 인증 요청이 401 bad 로 거절됐습니다 ({ip})", flush=True)
                probe = False
        if now >= deadline:
            break
        time.sleep(1)
    problems = []
    if not build_seen:
        problems.append(f"기록에 build {build} 가 나오지 않았습니다")
    if probe and probe_error:
        problems.append(probe_error)
    elif probe:
        # Wi-Fi 없이 쓰는 기기이거나 이 Python 에 네트워크 권한이 없을 수 있어 실패로 치지 않는다. 대신 확인하지 못했다고 적는다
        print(f"[soak] 경고: 기기 IP({ip})로 잘못된 인증 요청을 보내지 못했습니다 (거절 경로 미확인)", flush=True)
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG)
    parser.add_argument("--offset", type=int, help="이 바이트부터 본다 (기본: 지금 기록의 끝)")
    parser.add_argument("--seconds", type=float, default=DEFAULT_SECONDS)
    parser.add_argument("--build", help="이 build 가 기록에 나와야 통과")
    parser.add_argument("--probe", action="store_true", help="잘못된 인증 요청을 보내 거절 경로를 지나가게 한다")
    args = parser.parse_args()
    offset = log_size(args.log) if args.offset is None else args.offset
    problems = soak(args.log, offset, args.seconds, args.build, args.probe)
    for problem in problems:
        print(f"[soak] 실패: {problem}")
    if not problems:
        print(f"[soak] 통과: {args.seconds:g}초 동안 크래시 없음")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
