"""USB 케이블(시리얼 포트)로 통신한다. 형식은 펌웨어의 usb_link.h 참고."""

import json
import struct
import threading
import time
from pathlib import Path

import serial
from serial.tools import list_ports

from . import frames

ESPRESSIF_VID = 0x303A
# ESP32-S3 내장 USB(시리얼/JTAG). 같은 회사의 다른 기기(M5Stack UiFlow 등)는 다른 번호라 걸러진다.
USB_JTAG_PID = 0x1001
# 같은 번호를 쓰는 다른 ESP32 보드일 수도 있어서, 열고 나서 이 시간 안에 LLM Deck 의 응답이 없으면 다른 기기로 보고 놓아준다
CONFIRM_SECONDS = 10
# 한 번 확인한 기기의 시리얼 번호를 기억해 두고 먼저 고른다
KNOWN_FILE = Path.home() / ".llm-deck" / "usb_serial"
# 이번 실행에서 LLM Deck 이 아니라고 확인된 기기 (시리얼 번호)
_rejected: set[str] = set()


def _known_serial() -> str:
    try:
        return KNOWN_FILE.read_text().strip()
    except OSError:
        return ""


def find_port(fixed: str = "") -> tuple[str, str] | None:
    """(포트, 시리얼 번호). 설정에 포트를 적었으면 그것을 쓴다."""
    if fixed:
        return fixed, ""
    known = _known_serial()
    candidates = [
        p for p in list_ports.comports()
        if p.vid == ESPRESSIF_VID and p.pid == USB_JTAG_PID and (p.serial_number or "") not in _rejected
    ]
    candidates.sort(key=lambda p: (p.serial_number or "") != known)
    return (candidates[0].device, candidates[0].serial_number or "") if candidates else None


class UsbTransport:
    name = "USB"

    def __init__(self, cfg: dict, on_audio, on_event=None, on_photo=None, on_video=None):
        self.port_name = cfg.get("port", "")
        self.on_audio = on_audio
        self.on_event = on_event or (lambda event: None)
        self.on_photo = on_photo or (lambda offset, data: None)
        self.on_video = on_video or (lambda no, offset, total, ms, data: None)
        self.ser = None
        self.serial_number = ""
        self.opened_at = 0.0
        self.confirmed = False

    def _find_port(self) -> str | None:
        found = find_port(self.port_name)
        return found[0] if found else None

    def open(self) -> bool:
        found = find_port(self.port_name)
        if not found:
            return False
        port, self.serial_number = found
        try:
            # DTR/RTS 신호는 기본값(켜짐)으로 둔다. 끄고 열면 보드가 재시작된다.
            ser = serial.Serial(port, 115200, timeout=0.5)
        except serial.SerialException:
            return False
        self.ser = ser
        self.opened_at = time.time()
        # 설정에 포트를 직접 적었거나 전에 확인한 기기면 바로 믿는다
        self.confirmed = bool(self.port_name) or (self.serial_number and self.serial_number == _known_serial())
        threading.Thread(target=self._read_loop, args=(ser,), daemon=True).start()
        return True

    def _confirm(self) -> None:
        if self.confirmed:
            return
        self.confirmed = True
        if self.serial_number:
            try:
                KNOWN_FILE.parent.mkdir(parents=True, exist_ok=True)
                KNOWN_FILE.write_text(self.serial_number)
            except OSError:
                pass

    def connected(self) -> bool:
        # 케이블을 뽑으면 읽는 쪽이 오류로 끝나며 포트가 닫히거나, 포트 자체가 사라진다
        if not self.ser or not self.ser.is_open:
            return False
        return self.port_name != "" or any(p.device == self.ser.port for p in list_ports.comports())

    def send(self, message: dict) -> None:
        if not self.confirmed and time.time() - self.opened_at > CONFIRM_SECONDS:
            # LLM Deck 이 아닌 ESP32 기기다 (다른 프로젝트 보드 등). 놓아주고 다시 찾지 않는다.
            if self.serial_number:
                _rejected.add(self.serial_number)
            print(f"[USB] {self.ser.port if self.ser else ''} 는 LLM Deck 이 아닌 것 같아 놓아줍니다.")
            raise OSError("not an LLM Deck")
        try:
            self.ser.write(json.dumps(message, separators=(",", ":")).encode() + b"\n")
        except (serial.SerialException, AttributeError) as e:
            raise OSError(str(e)) from e

    def close(self) -> None:
        if self.ser:
            self.ser.close()
            self.ser = None

    def _read_loop(self, ser) -> None:
        """기기가 보내는 바이트에서 프레임을 골라낸다. 프레임이 아닌 부분은 로그 글자다."""
        buf = b""
        while ser.is_open:
            try:
                buf += ser.read(4096)
            except (serial.SerialException, OSError, TypeError):
                return
            if not self.confirmed and (b"[link]" in buf or b"\xa5\x5a" in buf):
                # LLM Deck 펌웨어만 내는 로그·묶음이 보이면 우리 기기다
                self._confirm()
            found, buf = frames.split(buf, self._print_log)
            for kind, body in found:
                frames.dispatch(self, kind, body)

    @staticmethod
    def _print_log(data: bytes, flush: bool = False) -> bytes:
        """완성된 줄만 출력하고, 아직 끝나지 않은 줄은 돌려준다."""
        *lines, rest = data.split(b"\n")
        if flush and rest:
            lines.append(rest)
            rest = b""
        for line in lines:
            text = line.decode(errors="replace").strip()
            # 연결 직후에는 잘린 소리 데이터가 섞여 올 수 있으므로 글자로 읽히는 줄만 보여준다
            if text and line.isascii() and text.isprintable():
                print(f"  기기> {text}")
        return rest
