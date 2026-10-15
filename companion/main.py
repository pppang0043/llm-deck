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
    uv run main.py --test-photo     카메라 없이 무늬 사진을 찍은 것처럼 해서 SD 카드·PC 저장을 시험한다
    uv run main.py --test-sound     기기가 마이크를 켜고 끌 때 PC 스피커로 나는 소리를 들어 본다
    uv run main.py --add-wifi       기기가 접속할 Wi-Fi 를 추가로 등록한다 (USB 로 연결한 상태에서)
    uv run main.py --clear-wifi     추가로 등록한 Wi-Fi 를 모두 지운다
    uv run main.py --sync-only      기기 없이, 이 PC 의 한도 사용률을 다른 PC 와 공유만 한다
    uv run main.py --setup-sync     공유에 쓸 비공개 gist 를 만든다 (처음 한 번)
    uv run main.py --install-statusline    Claude Code 한도 사용률을 받아오도록 설정한다 (PC 마다 한 번)
"""

import argparse
import getpass
import json
import socket
import sys
import time
import tomllib
from datetime import datetime
from pathlib import Path

import audio
import photos
import videos
import sounds
import statusline
import sync
from collectors import COLLECTORS, LIMITS
from collectors.fmt import limits_page
from transports import TRANSPORTS

INTERVAL = 30
INSTANCE_PORT = 4012  # 도우미가 하나만 돌게 하려고 잡는 PC 안쪽 포트
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
    # 기기에는 시계가 없어서(Wi-Fi 가 없으면 시각을 모른다) 보낼 때마다 현재 시각을 알려준다
    clock = {"now": int(time.time()), "utc_offset": time.localtime().tm_gmtoff}
    return {"updated": datetime.now().strftime("%H:%M"), "services": services, **clock, **extra}


_instance_lock = None


def claim_device() -> bool:
    """기기에 붙는 도우미는 PC 에 하나만 돈다. 이미 돌고 있으면 False.

    블루투스 연결은 기기가 하나만 받고 USB 포트도 한 프로그램만 열 수 있어서, 두 번째 도우미는
    기기를 "찾지 못했다"고만 나온다. 그 대신 무엇이 문제인지 알려 주려고 PC 안에서만 쓰는 포트를 잡는다.
    """
    global _instance_lock
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind(("127.0.0.1", INSTANCE_PORT))
        sock.listen(1)
    except OSError:
        sock.close()
        return False
    _instance_lock = sock
    return True


def usb_plugged(cfg: dict) -> bool:
    """기기가 USB 로 꽂혀 있는지 (포트가 보이는지만 본다)."""
    return TRANSPORTS["usb"](cfg.get("device", {}), lambda seq, pcm: None)._find_port() is not None


def connect(names: list[str], cfg: dict, on_audio, on_event, on_photo, on_video):
    for name in names:
        transport = TRANSPORTS[name](cfg.get("device", {}), on_audio, on_event, on_photo, on_video)
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
    parser.add_argument("--battery", type=float, metavar="V", help="멀티미터로 잰 배터리 전압(V)으로 기기를 보정한다")
    parser.add_argument("--camera", choices=["open", "shoot", "record", "close", "dump"], help="기기의 카메라 화면을 열기 / 찍기 / 닫기")
    parser.add_argument("--camera-mount", choices=["0", "1", "2", "3", "0m", "1m", "2m", "3m"],
                        help="사진이 돌아가 보일 때: 센서 방향(시계 방향 90도 횟수), 좌우가 뒤집히면 m 을 붙인다")
    parser.add_argument("--test-video", type=int, metavar="초", help="기기 카메라로 정한 초만큼 녹화해 저장한다")
    parser.add_argument("--links", metavar="usb=on,ble=off", help="기기의 연결 방식 켜고 끄기 (USB 로 연결했을 때만)")
    parser.add_argument("--view", choices=["home", "usage", "info", "settings"], help="기기 화면 바꾸기")
    parser.add_argument("--screenshot", action="store_true", help="기기 화면을 찍어 사진 폴더에 저장한다 (진단용)")
    parser.add_argument("--test-photo", action="store_true", help="카메라 없이 무늬 사진으로 사진 저장·전송을 시험한다")
    parser.add_argument("--test-sound", action="store_true", help="마이크를 켜고 끌 때 나는 소리를 들어 본다")
    parser.add_argument("--sync-only", action="store_true", help="기기 없이 한도 사용률 공유만 한다")
    parser.add_argument("--setup-sync", action="store_true", help="공유에 쓸 비공개 gist 를 만든다")
    parser.add_argument("--log", metavar="FILE", help="출력을 파일에 덧붙여 쓴다 (창 없이 자동 실행할 때)")
    args = parser.parse_args()
    if args.log:
        log = open(Path(args.log).expanduser(), "a", encoding="utf-8", buffering=1)
        sys.stdout = sys.stderr = log
    if args.install_statusline:
        return statusline.install()
    if args.uninstall_statusline:
        return statusline.uninstall()
    cfg = load_config()

    if args.test_sound:
        speaker = sounds.Sounds(cfg.get("sounds", {}))
        for name in ("mic_on", "mic_off"):
            print(f"[sound] {name}")
            speaker.play(name)
            time.sleep(1)
        return
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

    if not claim_device():
        log = Path.home() / ".llm-deck" / "companion.log"
        print("[device] 이 PC 에서 이미 다른 도우미가 기기에 연결되어 있습니다 (로그인할 때 자동으로 켜진 것일 수 있습니다).")
        print(f"         그 도우미의 기록: {log}")
        print("         여기서 직접 실행하려면 먼저 그 도우미를 끄세요.")
        print("         Windows: 작업 관리자에서 python.exe 종료 / Mac: launchctl bootout gui/$(id -u)/com.llmdeck.companion")
        return
    link = args.link or cfg.get("device", {}).get("link", "auto")
    names = list(TRANSPORTS) if link == "auto" else [link]
    sink = audio.Recorder(args.record) if args.record else audio.Player(cfg.get("audio", {}))
    speaker = sounds.Sounds(cfg.get("sounds", {}))
    receiver = photos.PhotoReceiver(cfg.get("photos", {}))
    movies = videos.VideoReceiver(cfg.get("videos", {}))

    def on_event(event: dict) -> None:
        speaker.on_event(event)
        receiver.on_event(event)
        movies.on_event(event)

    transport = None
    first = True
    while True:
        if transport is None:
            transport = connect(names, cfg, sink.push, on_event, receiver.on_chunk, movies.on_chunk)
        if transport:
            message = collect_all(cfg)
            if first and args.mic:
                message["mic"] = args.mic == "on"
            if first and args.test_photo:
                message["photo_test"] = True
            if first and args.camera:
                message["camera"] = args.camera
            if first and args.links:
                message["links"] = {k: v == "on" for k, v in (item.split("=") for item in args.links.split(","))}
            if first and args.view:
                message["view"] = args.view
            if first and args.screenshot:
                message["screenshot"] = True
            if first and args.test_video:
                message["camera"] = "record"
            if first and args.camera_mount:
                rot = int(args.camera_mount[0])
                message["camera_mount"] = {"rot": rot, "mirror": args.camera_mount.endswith("m")}
            if first and args.battery is not None:
                message["battery_mv"] = round(args.battery * 1000)
            try:
                transport.send(message)
                print(f"[device] {message['updated']} 사용량 전송 ({len(message['services'])}개 서비스)")
                first = False
            except OSError as e:
                print(f"[device] 전송 실패: {e}")
                transport.close()
                transport = None
            # 블루투스·Wi-Fi 로 붙어 있는데 USB 케이블이 꽂히면 USB 로 갈아탄다 (더 빠르고, 기기도 USB 를 우선한다)
            if transport and link == "auto" and transport.name != "USB" and usb_plugged(cfg):
                print(f"[device] USB 케이블이 연결되어 {transport.name} 에서 USB 로 바꿉니다.")
                transport.close()
                transport = None
                continue
        else:
            # 기기가 없어도 이 PC 의 한도 사용률은 계속 공유한다 (기기가 클라우드로 읽어 갈 수 있게)
            collect_limits(cfg)
        if args.test_video and transport and not first:
            # 정해진 초만큼 녹화하고 멈춘 뒤, 파일이 다 쓰일 때까지 기다린다
            time.sleep(args.test_video)
            transport.send({"camera": "record"})
            for _ in range(30):
                time.sleep(1)
                if movies.writer is None:
                    break
            transport.send({"camera": "close"})
            time.sleep(1)
            transport.close()
            return
        if (args.camera in ("open", "close") or args.camera_mount or args.links) and transport and not first:
            time.sleep(2)
            transport.close()
            return
        if args.view and not args.screenshot and transport and not first:
            time.sleep(1)
            transport.close()
            return
        if (args.test_photo or args.screenshot or args.camera in ("shoot", "dump")) and transport and not first:
            # 사진이 다 올 때까지 기다린다 (블루투스는 수 초)
            for _ in range(60):
                time.sleep(1)
                if receiver.buf is None and receiver.name:
                    break
            transport.close()
            return
        if args.battery is not None and transport and not first:
            print(f"[device] 배터리 전압 {args.battery:.3f}V 로 보정했습니다." if args.battery else "[device] 배터리 보정을 지웠습니다.")
            time.sleep(1)
            transport.close()
            return
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
