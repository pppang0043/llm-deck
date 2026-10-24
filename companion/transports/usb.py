"""USB 케이블(시리얼 포트)로 통신한다. 형식은 펌웨어의 usb_link.h 참고."""

import json
import struct
import threading
import time
from pathlib import Path

import serial
from serial.tools import list_ports

from . import auth, frames

ESPRESSIF_VID = 0x303A
# ESP32-S3 내장 USB(시리얼/JTAG). 같은 회사의 다른 기기(M5Stack UiFlow 등)는 다른 번호라 걸러진다.
USB_JTAG_PID = 0x1001
# 같은 번호를 쓰는 다른 ESP32 보드일 수도 있어서, 열고 나서 이 시간 안에 LLM Deck 의 응답이 없으면 다른 기기로 보고 놓아준다
CONFIRM_SECONDS = 10
# 한 번 확인한 기기의 시리얼 번호를 기억해 두고 먼저 고른다
KNOWN_FILE = Path.home() / ".llm-deck" / "usb_serial"
# 이번 실행에서 LLM Deck 이 아니라고 확인된 기기 (시리얼 번호)
_rejected: set[str] = set()
# 펌웨어는 돌고 있는 동안 1분마다 배터리 기록을 보낸다 (battery.cpp). 이만큼 한 바이트도 오지 않으면
# 펌웨어가 돌지 않는 것이다 (도우미를 다시 켜거나 펌웨어를 올린 뒤 다운로드 모드 등에 멈춘 적이 있다). 기기를 다시 켠다
SILENT_SECONDS = 150
# 다시 켜도 안 살아나면 되풀이하지 않게, 다시 켜는 것은 이 간격에 한 번까지
RESET_GAP_SECONDS = 600
_last_reset = 0.0


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
        self._on_event = on_event or (lambda event: None)
        self.on_photo = on_photo or (lambda offset, data: None)
        self.on_video = on_video or (lambda no, offset, total, ms, data: None)
        self.ser = None
        self.serial_number = ""
        self.opened_at = 0.0
        self.last_rx = 0.0  # 기기에서 마지막으로 무언가 받은 시각
        self.confirmed = False
        self.on_confirm = None  # LLM Deck 으로 확인되면 부른다 (main.py)
        # 메인 루프와 미러링 창(누르기)이 따로 보내므로 한 줄씩 차례로 쓴다
        self._send_lock = threading.Lock()
        # Wi-Fi 인증 키를 받았는지, 마지막으로 달라고 한 시각 (auth.py). 케이블로 연결했을 때만 받는다
        self.paired = False
        self._pair_asked = 0.0

    def on_event(self, event) -> None:
        pair = event.get("pair") if isinstance(event, dict) else None
        if isinstance(pair, dict):
            # 키는 기록에 남기지 않고 저장만 한다
            self.paired = True
            try:
                if auth.save_key(str(pair.get("id", "")), str(pair.get("key", ""))):
                    print(f"[USB] 기기({pair.get('id')})의 Wi-Fi 인증 키를 받았습니다. 이제 Wi-Fi 로도 연결할 수 있습니다.")
            except OSError as e:
                print(f"[USB] Wi-Fi 인증 키를 저장하지 못했습니다: {e}")
            return
        self._on_event(event)

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
            # 기기가 멈춰 받아 가지 않으면 쓰기가 끝없이 기다리므로 5초에서 끊고 다시 연결한다
            ser = serial.Serial(port, 115200, timeout=0.5, write_timeout=5)
        except serial.SerialException:
            return False
        self.ser = ser
        self.opened_at = self.last_rx = time.time()
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
        self.take_confirm()

    def take_confirm(self) -> None:
        """on_confirm 을 한 번만 부른다"""
        callback, self.on_confirm = self.on_confirm, None
        if callback:
            callback()

    def connected(self) -> bool:
        # 케이블을 뽑으면 읽는 쪽이 오류로 끝나며 포트가 닫히거나, 포트 자체가 사라진다
        if not self.ser or not self.ser.is_open:
            return False
        if self._silent():
            self._reset_device()
            return False
        return self.port_name != "" or any(p.device == self.ser.port for p in list_ports.comports())

    def _silent(self) -> bool:
        """LLM Deck 이라고 아는 기기(기억해 둔 시리얼 번호)가 오래 아무것도 보내지 않는지"""
        known = self.serial_number and self.serial_number == _known_serial()
        return bool(known) and time.time() - self.last_rx > SILENT_SECONDS and time.time() - _last_reset > RESET_GAP_SECONDS

    def _reset_device(self) -> None:
        """기기를 다시 켠다. esptool 의 보통 재시작과 같다: DTR 을 끈 채(다운로드 모드로 가지 않게) RTS 로 잠깐 리셋"""
        global _last_reset
        _last_reset = time.time()
        port = self.ser.port
        print(f"[USB] 기기가 {SILENT_SECONDS}초 동안 아무것도 보내지 않아 다시 켭니다 ({port})")
        self.close()
        try:
            ser = serial.Serial()
            ser.port, ser.baudrate = port, 115200
            ser.dtr = ser.rts = False  # 열 때 신호가 바뀌지 않게 먼저 정한다
            ser.open()
            ser.rts = True
            time.sleep(0.1)
            ser.rts = False
            ser.close()
        except (serial.SerialException, OSError) as e:
            print(f"[USB] 기기를 다시 켜지 못했습니다: {e}")

    def send(self, message: dict) -> None:
        if not self.confirmed and time.time() - self.opened_at > CONFIRM_SECONDS:
            # LLM Deck 이 아닌 ESP32 기기다 (다른 프로젝트 보드 등). 놓아주고 다시 찾지 않는다.
            if self.serial_number:
                _rejected.add(self.serial_number)
            print(f"[USB] {self.ser.port if self.ser else ''} 는 LLM Deck 이 아닌 것 같아 놓아줍니다.")
            raise OSError("not an LLM Deck")
        try:
            with self._send_lock:
                self.ser.write(json.dumps(message, separators=(",", ":")).encode() + b"\n")
                if not self.paired and time.time() - self._pair_asked > 30:
                    self._pair_asked = time.time()
                    self.ser.write(b'{"pair":true}\n')
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
                data = ser.read(4096)
            except (serial.SerialException, OSError, TypeError):
                return
            if data:
                self.last_rx = time.time()
                buf += data
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
