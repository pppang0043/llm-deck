"""LLM Deck PC 도우미.

    uv run main.py                  사용량을 30초마다 기기로 보내고 마이크 소리를 받는다
    uv run main.py --link usb       연결 방식을 고른다: usb / ble / wifi (기본: 되는 것을 자동 선택)
    uv run main.py --dry-run        기기로 보내지 않고, 보낼 내용만 출력한다
    uv run main.py --record a.wav   마이크 소리를 10초간 WAV 로 저장하고 끝낸다 (수신 확인용)
    uv run main.py --mic on         기기의 마이크를 PC 에서 켠다 (off 로 끈다)
    uv run main.py --add-wifi       기기가 접속할 Wi-Fi 를 추가로 등록한다 (USB 로 연결한 상태에서)
    uv run main.py --clear-wifi     추가로 등록한 Wi-Fi 를 모두 지운다
    uv run main.py --sync-only      기기 없이, 이 PC 의 한도 사용률을 다른 PC 와 공유만 한다
    uv run main.py --setup-sync     공유에 쓸 비공개 gist 를 만든다 (처음 한 번)
    uv run main.py --install-statusline    Claude Code 한도 사용률을 받아오도록 설정한다 (PC 마다 한 번)
"""

import argparse
import getpass
import json
import time
import tomllib
from datetime import datetime
from pathlib import Path

import audio
import statusline
import sync
from collectors import COLLECTORS, LIMITS
from collectors.fmt import limits_page
from transports import TRANSPORTS

INTERVAL = 30
RECORD_SECONDS = 10


def load_config() -> dict:
    path = Path(__file__).parent / "config.toml"
    if not path.exists():
        return {}
    with path.open("rb") as f:
        return tomllib.load(f)


# 설정 섹션 이름 -> (마지막 호출 시각, 마지막 결과)
_last: dict[str, tuple[float, dict | None]] = {}


_sync: sync.Sync | None = None


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
        except Exception as e:
            print(f"[{section}] 수집 실패: {e}")
    # 공유 저장소에는 여러 PC 의 값을 합친 것을 올린다 (화면에 무엇을 보여주는지와는 별개)
    shared = _sync.merge(local)
    pages = []
    for section, module in enabled:
        snapshot = local.get(section) or shared.get(section)
        if snapshot:
            pages.append(limits_page(module.NAME, snapshot))
        elif hasattr(module, "hint") and module.hint():
            pages.append(module.hint())
    return pages


def collect_all(cfg: dict) -> dict:
    services = collect_limits(cfg)
    extra = {"sync_url": _sync.public_url} if _sync.public_url else {}
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
    return {"updated": datetime.now().strftime("%H:%M"), "services": services, **extra}


def connect(names: list[str], cfg: dict, on_audio):
    for name in names:
        transport = TRANSPORTS[name](cfg.get("device", {}), on_audio)
        if transport.open():
            print(f"[device] {transport.name} 로 연결했습니다.")
            return transport
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
    transport.send(message)
    time.sleep(1)
    transport.close()
    print("[device] 등록했습니다. 최대 5개까지 기억합니다." if add else "[device] 지웠습니다. 기기를 다시 켜면 적용됩니다.")


def main() -> None:
    parser = argparse.ArgumentParser(description="LLM Deck PC companion")
    parser.add_argument("--link", choices=["auto", *TRANSPORTS], help="연결 방식 (기본: 설정 파일 값 또는 auto)")
    parser.add_argument("--dry-run", action="store_true", help="기기로 보내지 않고 보낼 내용만 출력")
    parser.add_argument("--record", metavar="WAV", help=f"마이크 소리를 {RECORD_SECONDS}초간 WAV 파일로 저장하고 종료")
    parser.add_argument("--mic", choices=["on", "off"], help="기기의 마이크를 켜거나 끈다")
    parser.add_argument("--install-statusline", action="store_true", help="Claude Code 한도 사용률을 받아오도록 설정")
    parser.add_argument("--uninstall-statusline", action="store_true", help="위 설정을 원래대로 되돌린다")
    parser.add_argument("--add-wifi", action="store_true", help="기기가 접속할 Wi-Fi 를 추가로 등록 (USB)")
    parser.add_argument("--clear-wifi", action="store_true", help="추가로 등록한 Wi-Fi 를 모두 지운다 (USB)")
    parser.add_argument("--sync-only", action="store_true", help="기기 없이 한도 사용률 공유만 한다")
    parser.add_argument("--setup-sync", action="store_true", help="공유에 쓸 비공개 gist 를 만든다")
    args = parser.parse_args()
    if args.install_statusline:
        return statusline.install()
    if args.uninstall_statusline:
        return statusline.uninstall()
    cfg = load_config()

    if args.add_wifi or args.clear_wifi:
        return wifi_setup(cfg, add=args.add_wifi)
    if args.setup_sync:
        return sync.create(cfg.get("sync", {}))
    if args.sync_only:
        print("[sync] 이 PC 의 한도 사용률을 공유합니다. (Ctrl+C 로 종료)")
        while True:
            collect_limits(cfg)
            time.sleep(INTERVAL)

    if args.dry_run:
        print(json.dumps(collect_all(cfg), indent=2, ensure_ascii=False))
        return

    link = args.link or cfg.get("device", {}).get("link", "auto")
    names = list(TRANSPORTS) if link == "auto" else [link]
    sink = audio.Recorder(args.record) if args.record else audio.Player(cfg.get("audio", {}))

    transport = None
    first = True
    while True:
        if transport is None:
            transport = connect(names, cfg, sink.push)
        if transport:
            message = collect_all(cfg)
            if first and args.mic:
                message["mic"] = args.mic == "on"
            try:
                transport.send(message)
                print(f"[device] {message['updated']} 사용량 전송 ({len(message['services'])}개 서비스)")
                first = False
            except OSError as e:
                print(f"[device] 전송 실패: {e}")
                transport.close()
                transport = None
        if args.record:
            if transport:
                print(f"[audio] {RECORD_SECONDS}초간 녹음합니다. 기기의 마이크가 켜져 있어야 합니다.")
                time.sleep(RECORD_SECONDS)
                transport.close()
            sink.close()
            return
        time.sleep(INTERVAL if transport else 5)


if __name__ == "__main__":
    main()
