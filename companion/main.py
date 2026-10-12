"""LLM Deck PC 도우미.

    uv run main.py              사용량을 30초마다 기기로 보내고 마이크 소리를 받는다
    uv run main.py --dry-run    기기로 보내지 않고, 보낼 내용만 출력한다
    uv run main.py --record test.wav   마이크 소리를 10초간 WAV 로 저장한다 (수신 확인용)
"""

import argparse
import json
import socket
import time
import tomllib
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

import audio
from collectors import COLLECTORS

INTERVAL = 30
SERVICE_TYPE = "_llm-deck._tcp.local."
HOSTNAME = "llm-deck.local"


def load_config() -> dict:
    path = Path(__file__).parent / "config.toml"
    if not path.exists():
        return {}
    with path.open("rb") as f:
        return tomllib.load(f)


def find_device(cfg: dict) -> str | None:
    """기기의 IP 를 찾는다. 설정에 적힌 주소 > mDNS 서비스 검색 > llm-deck.local 순."""
    host = cfg.get("device", {}).get("host")
    if host:
        return host

    from zeroconf import Zeroconf

    zc = Zeroconf()
    try:
        info = zc.get_service_info(SERVICE_TYPE, f"llm-deck.{SERVICE_TYPE}", timeout=3000)
        if info and info.parsed_addresses():
            return info.parsed_addresses()[0]
    finally:
        zc.close()
    try:
        return socket.gethostbyname(HOSTNAME)
    except OSError:
        return None


# 설정 섹션 이름 -> (마지막 호출 시각, 마지막 결과)
_last: dict[str, tuple[float, dict | None]] = {}


def collect_all(cfg: dict) -> dict:
    services = []
    for section, module, min_interval in COLLECTORS:
        called_at, result = _last.get(section, (0.0, None))
        if time.time() - called_at >= min_interval:
            try:
                result = module.collect(cfg.get(section, {}))
            except Exception as e:  # 한 서비스가 실패해도 나머지는 계속 보여준다
                print(f"[{section}] 수집 실패: {e}")
            _last[section] = (time.time(), result)
        if result:
            services.append(result)
    return {"updated": datetime.now().strftime("%H:%M"), "services": services}


def send(host: str, payload: dict) -> dict:
    req = urllib.request.Request(
        f"http://{host}/usage",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.load(resp)


def main() -> None:
    parser = argparse.ArgumentParser(description="LLM Deck PC companion")
    parser.add_argument("--dry-run", action="store_true", help="기기로 보내지 않고 보낼 내용만 출력")
    parser.add_argument("--record", metavar="WAV", help="마이크 소리를 10초간 WAV 파일로 저장")
    args = parser.parse_args()
    cfg = load_config()

    if args.dry_run:
        print(json.dumps(collect_all(cfg), indent=2, ensure_ascii=False))
        return

    host = None
    if not args.record:
        audio.start(cfg.get("audio", {}))

    recorded = False
    while True:
        if host is None:
            host = find_device(cfg)
            print(f"[device] {host} 에 연결합니다." if host else "[device] 기기를 찾지 못했습니다. 다시 시도합니다.")
        if host:
            try:
                # 기기는 마지막으로 데이터를 보낸 PC 로 마이크 소리를 보내므로, 녹음 전에도 한 번 보내야 한다
                state = send(host, collect_all(cfg))
                print(f"[device] 전송 완료 (마이크 {'켜짐' if state.get('mic') else '꺼짐'})")
            except (urllib.error.URLError, OSError) as e:
                print(f"[device] 전송 실패: {e}")
                host = None
        if args.record:
            if host and not recorded:
                print("[audio] 10초간 녹음합니다. 기기의 마이크를 켜고 말해보세요.")
                audio.record(args.record, 10)
            return
        time.sleep(INTERVAL)


if __name__ == "__main__":
    main()
