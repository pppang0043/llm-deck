"""USB 케이블(시리얼 포트)로 통신한다. 형식은 펌웨어의 usb_link.h 참고."""

import json
import struct
import threading

import serial
from serial.tools import list_ports

from . import frames

ESPRESSIF_VID = 0x303A


class UsbTransport:
    name = "USB"

    def __init__(self, cfg: dict, on_audio, on_event=None, on_photo=None, on_video=None):
        self.port_name = cfg.get("port", "")
        self.on_audio = on_audio
        self.on_event = on_event or (lambda event: None)
        self.on_photo = on_photo or (lambda offset, data: None)
        self.on_video = on_video or (lambda no, offset, total, ms, data: None)
        self.ser = None

    def _find_port(self) -> str | None:
        if self.port_name:
            return self.port_name
        for port in list_ports.comports():
            if port.vid == ESPRESSIF_VID:
                return port.device
        return None

    def open(self) -> bool:
        port = self._find_port()
        if not port:
            return False
        try:
            # DTR/RTS 신호는 기본값(켜짐)으로 둔다. 끄고 열면 보드가 재시작된다.
            ser = serial.Serial(port, 115200, timeout=0.5)
        except serial.SerialException:
            return False
        self.ser = ser
        threading.Thread(target=self._read_loop, args=(ser,), daemon=True).start()
        return True

    def send(self, message: dict) -> None:
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
