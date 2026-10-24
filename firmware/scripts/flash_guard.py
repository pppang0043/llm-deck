"""USB 업로드: 실행 중인 프로토콜과 실제 이미지의 ELF SHA 를 전후 검증한다."""
import argparse
import fcntl
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid
from contextlib import contextmanager
from pathlib import Path

from build_manifest import companion_protocol, validate_artifact, validate_plan, validate_running, write_json, staged_artifact, upload_spec

FIRMWARE = Path(__file__).resolve().parents[1]


@contextmanager
def _directory_lock(state, timeout=600):
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    lock = state / "flash.lock"
    deadline = time.monotonic() + timeout
    while True:
        try:
            lock.mkdir()
            break
        except FileExistsError:
            try:
                owner = int((lock / "pid").read_text())
                if owner <= 0:
                    raise ValueError()
                os.kill(owner, 0)
            except PermissionError:
                pass  # 다른 사용자의 살아 있는 프로세스
            except (FileNotFoundError, ValueError, ProcessLookupError):
                # mkdir 와 pid 기록 사이에 다른 업로더가 들어올 수 있다.
                if lock.exists() and time.time() - lock.stat().st_mtime > 10:
                    shutil.rmtree(lock)
                    continue
            if time.monotonic() >= deadline:
                raise RuntimeError("다른 업로더의 잠금이 유지되고 있습니다")
            time.sleep(.5)
    (lock / "pid").write_text(str(os.getpid()))
    try:
        yield
    finally:
        (lock / "pid").unlink(missing_ok=True)
        lock.rmdir()


@contextmanager
def flash_lock(state, timeout=600):
    # 새 업로더끼리는 OS 잠금으로 직렬화하고, 기존 flash.sh 와는 디렉터리 잠금도 공유한다.
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (state / "flash.guard.lock").open("a") as guard:
        deadline = time.monotonic() + timeout
        while True:
            try:
                fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise RuntimeError("다른 업로더의 OS 잠금이 유지되고 있습니다")
                time.sleep(.5)
        try:
            with _directory_lock(state, max(0, deadline - time.monotonic())):
                yield
        finally:
            fcntl.flock(guard, fcntl.LOCK_UN)


def run_pio(pio, *args, expected=None):
    process_env = dict(os.environ)
    if expected:
        process_env.update({"LLMDECK_EXPECTED_BUILD": expected["build"],
                            "LLMDECK_EXPECTED_SOURCE": expected["source_digest"],
                            "LLMDECK_EXPECTED_SHA": expected["firmware_sha256"],
                            "LLMDECK_EXPECTED_IMAGES": json.dumps(upload_spec(expected["images"])),
                            "LLMDECK_STAGED_MANIFEST": expected["staged_manifest"],
                            "LLMDECK_UPLOAD_PORT": args[args.index("--upload-port") + 1]})
    process = subprocess.Popen([pio, "run", "-e", "llm-deck", *args], cwd=FIRMWARE,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True, env=process_env)
    try:
        for line in process.stdout:
            if "auth=" not in line and "Namespace" not in line and re.search(r"error|Error|SUCCESS|FAILED|warning: unused", line):
                print(line.rstrip(), flush=True)
        return process.wait() == 0
    except BaseException:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        raise



def find_device(port=None, serial_number=None):
    from serial.tools import list_ports
    ports = [p for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001]
    if port:
        ports = [p for p in ports if p.device == port]
    if serial_number:
        ports = [p for p in ports if p.serial_number == serial_number]
    if len(ports) != 1:
        raise RuntimeError("등록된 USB 기기 하나를 식별하지 못했습니다. 포트와 기기 연결을 확인하세요")
    device = ports[0]
    identity = (device.serial_number or "").replace(":", "").lower()
    if not re.fullmatch(r"[0-9a-f]{12}", identity):
        raise RuntimeError("USB 기기의 고유 번호를 확인하지 못했습니다")
    return device.device, device.serial_number, identity


def read_events(connection, seconds):
    deadline = time.monotonic() + seconds
    buf = b""
    while time.monotonic() < deadline:
        buf += connection.read(4096)
        while len(buf) >= 5:
            at = buf.find(b"\xa5\x5a")
            if at < 0:
                buf = buf[-1:]
                break
            buf = buf[at:]
            if len(buf) < 5:
                break
            length = int.from_bytes(buf[3:5], "little")
            if buf[2] not in (1, 2, 3, 4, 5, 6) or length > 2048:
                buf = buf[2:]  # 연결 직후 잘린 오디오/로그 안의 우연한 매직 바이트
                continue
            if len(buf) < length + 5:
                break
            kind, body, buf = buf[2], buf[5:5 + length], buf[5 + length:]
            if kind == 2:
                try:
                    event = json.loads(body)
                    if isinstance(event, dict):
                        yield event
                except ValueError:
                    pass


def probe(port, *, legacy_proto=None, timeout=3):
    import serial
    with serial.Serial(port, 115200, timeout=.2, write_timeout=2, exclusive=True) as connection:
        connection.reset_input_buffer()
        challenge = uuid.uuid4().hex
        connection.write(json.dumps({"firmware_info": challenge}).encode() + b"\n")
        for event in read_events(connection, timeout):
            if event.get("probe") == challenge and type(event.get("fw_proto")) is int:
                return event
        if legacy_proto is not None:
            # 최초 도입 전 펌웨어의 기존 버전 응답. 업로드 뒤 검증에는 이 경로를 쓰지 않는다.
            connection.write(json.dumps({"services": [], "ask_fw": True, "host": "upload-check",
                                         "helper": {"proto": legacy_proto}}).encode() + b"\n")
            for event in read_events(connection, timeout):
                if type(event.get("fw_proto")) is int:
                    return event
    raise RuntimeError("기기의 새 버전 응답을 받지 못했습니다")


class Helper:
    def __init__(self, state):
        self.state = state
        self.flag = state / "flashing"
        self.label = f"gui/{os.getuid()}/com.llmdeck.companion"
        self.plist = Path.home() / "Library/LaunchAgents/com.llmdeck.companion.plist"
        self.app = Path.home() / "Applications/LLM Deck.app"
        self.was_loaded = self.was_app = False

    def stop(self):
        self.flag.touch()
        self.was_loaded = subprocess.run(["launchctl", "print", self.label], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        pattern = re.escape(str(self.app)) + r"/Contents/MacOS/"
        self.was_app = subprocess.run(["pgrep", "-f", pattern], stdout=subprocess.DEVNULL).returncode == 0
        if self.was_loaded:
            subprocess.run(["launchctl", "bootout", self.label], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if self.was_app:
            subprocess.run(["pkill", "-f", pattern], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def restore(self):
        self.flag.unlink(missing_ok=True)
        if self.was_loaded:
            restored = subprocess.run(["launchctl", "bootstrap", f"gui/{os.getuid()}", str(self.plist)], capture_output=True)
            if restored.returncode:
                raise RuntimeError("도우미 자동 실행을 복원하지 못했습니다")
        elif self.was_app:
            subprocess.run(["open", "-g", str(self.app)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?")
    parser.add_argument("--allow-worktree-switch", action="store_true", help="다른 worktree 의 변경을 확인한 뒤 업로드 허용")
    parser.add_argument("--initial-flash", action="store_true", help="배포 이력이 없고 펌웨어 응답이 없는 새 기기의 최초 USB 설치")
    args = parser.parse_args()
    state = Path.home() / ".llm-deck"
    pio = os.environ.get("PIO", str(Path.home() / ".local/bin/pio"))
    with flash_lock(state):
        if not run_pio(pio):
            raise RuntimeError("빌드 실패. 업로드하지 않았습니다")
        build = FIRMWARE / ".pio/build/llm-deck"
        manifest = json.loads((build / "build-manifest.json").read_text())
        binary = build / "firmware.bin"
        validate_artifact(FIRMWARE, manifest, binary)
        companion = companion_protocol(FIRMWARE)
        known = (state / "usb_serial").read_text().strip() if (state / "usb_serial").exists() else None
        port, serial_number, identity = find_device(args.port, None if args.port else known)
        record_path = state / "firmware-deployments.json"
        records = json.loads(record_path.read_text()) if record_path.exists() else {}
        if not isinstance(records, dict):
            raise ValueError("업로드 이력 형식이 올바르지 않습니다. 자동 초기화하지 않습니다")
        previous = records.get(identity, {})
        validate_plan(manifest, companion, {"fw_proto": 0}, previous, FIRMWARE.parent,
                      allow_worktree_switch=args.allow_worktree_switch)
        with staged_artifact(FIRMWARE, manifest, binary) as (manifest, binary):
            helper = Helper(state)
            try:
                helper.stop()
                for _ in range(40):
                    if not subprocess.run(["lsof", "-t", port], capture_output=True).stdout.strip():
                        break
                    time.sleep(.25)
                else:
                    raise RuntimeError("다른 프로그램이 USB 포트를 사용 중입니다")
                try:
                    current = probe(port, legacy_proto=companion)
                except RuntimeError:
                    if not args.initial_flash or previous:
                        raise
                    current = {"fw_proto": 0}
                    print("[flash] 명시적인 최초 설치: 실행 펌웨어 응답 없음", flush=True)
                if current.get("id", identity) != identity:
                    raise ValueError("USB 고유 번호와 실행 중인 기기가 다릅니다")
                validate_plan(manifest, companion, current, previous, FIRMWARE.parent,
                              allow_worktree_switch=args.allow_worktree_switch)
                print(f"[flash] 검증: 기기 proto {current['fw_proto']} → {manifest['proto']}, 대상 build {manifest['build']}", flush=True)
                validate_artifact(FIRMWARE, manifest, binary)
                for attempt in range(2):
                    if run_pio(pio, "-t", "upload", "--upload-port", port, expected=manifest):
                        break
                    if attempt == 1:
                        raise RuntimeError("업로드 실패")
                    time.sleep(2)
                    port, _, _ = find_device(serial_number=serial_number)
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    try:
                        port, _, _ = find_device(serial_number=serial_number)
                        running = probe(port)
                        validate_running(manifest, running, identity)
                        break
                    except (OSError, RuntimeError):
                        time.sleep(1)
                else:
                    raise RuntimeError("업로드 후 실행 빌드를 확인하지 못했습니다")
                validate_artifact(FIRMWARE, manifest, binary)
                records[identity] = {"proto": manifest["proto"], "build": manifest["build"],
                                     "root": str(FIRMWARE.parent.resolve()), "source_digest": manifest["source_digest"]}
                write_json(record_path, records)
                print(f"[flash] 완료: proto {running['fw_proto']}, build {running['build']} (실제 기기 일치)", flush=True)
            finally:
                helper.restore()


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *args: (_ for _ in ()).throw(KeyboardInterrupt()))
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"[flash] 중단: {error}", file=sys.stderr)
        sys.exit(1)
