"""기기가 녹화한 동영상을 받아 MOV 파일로 저장한다.

기기는 640x480 JPEG 를 한 장씩 보낸다 (USB 프레임 또는 Wi-Fi TCP). 받은 JPEG 를 다시 압축하지 않고
그대로 QuickTime MOV(Motion JPEG)에 담는다. 화면 방향에 맞게 돌리는 정보는 파일 머리에 적어서,
재생기가 바로 세워 보여준다. 장마다 찍힌 시각을 그대로 써서 재생 속도가 실제와 같다.

    {"video": {"start": "VID_...mov", "turns": 1}}            녹화 시작 (turns: 시계 방향 90도 횟수)
    on_chunk(no, offset, total, ms, data)                       한 장의 조각
    {"video": {"end": "VID_...mov", "frames": N, "dropped": M}} 녹화 끝
저장 위치는 설정의 [videos] dir (기본: Mac 은 "동영상(Movies)", Windows 는 "비디오(Videos)" 아래 "LLM Deck").
"""

import struct
import sys
import threading
from pathlib import Path

from photos import open_new

DEFAULT_DIR = Path.home() / ("Movies" if sys.platform == "darwin" else "Videos") / "LLM Deck"
TIMESCALE = 1000  # ms 단위


def _box(kind: bytes, *parts: bytes) -> bytes:
    body = b"".join(parts)
    return struct.pack(">I", 8 + len(body)) + kind + body


def _matrix(turns: int, w: int, h: int) -> bytes:
    """QuickTime 변환 행렬 (a b u / c d v / x y w). a~d, x, y 는 16.16, u v w 는 2.30 고정소수점."""
    one = 0x10000
    a, b, c, d, x, y = {
        0: (one, 0, 0, one, 0, 0),
        1: (0, one, -one, 0, h * one, 0),
        2: (-one, 0, 0, -one, w * one, h * one),
        3: (0, -one, one, 0, 0, w * one),
    }[turns & 3]
    return struct.pack(">9i", a, b, 0, c, d, 0, x, y, 0x40000000)


class MovWriter:
    """Motion JPEG 를 담는 최소한의 QuickTime MOV. 장은 바로바로 파일에 쓰고, 목차(moov)는 끝에 쓴다."""

    def __init__(self, path: Path, turns: int, f=None):
        self.path = path
        self.turns = turns
        self.f = f or path.open("wb")
        self.f.write(_box(b"ftyp", b"qt  ", struct.pack(">I", 0x20050300), b"qt  "))
        self.mdat_at = self.f.tell()
        # 크기가 4GB 를 넘을 수 있어 64비트 크기 칸을 쓴다
        self.f.write(struct.pack(">I", 1) + b"mdat" + struct.pack(">Q", 0))
        self.offsets: list[int] = []
        self.sizes: list[int] = []
        self.times: list[int] = []
        self.width = self.height = 0

    def add(self, jpeg: bytes, ms: int) -> None:
        if not self.width:
            self.width, self.height = _jpeg_size(jpeg)
        self.offsets.append(self.f.tell())
        self.sizes.append(len(jpeg))
        self.times.append(ms)
        self.f.write(jpeg)

    def close(self) -> int:
        end = self.f.tell()
        self.f.seek(self.mdat_at + 8)
        self.f.write(struct.pack(">Q", end - self.mdat_at))
        self.f.seek(end)
        if self.sizes:
            self.f.write(self._moov())
        self.f.close()
        return len(self.sizes)

    def _moov(self) -> bytes:
        n = len(self.sizes)
        # 장마다 머무는 시간 = 다음 장과의 시각 차이. 마지막 장은 평균으로.
        gaps = [max(1, self.times[i + 1] - self.times[i]) for i in range(n - 1)]
        gaps.append(round(sum(gaps) / len(gaps)) if gaps else 100)
        duration = sum(gaps)
        stts_entries = []
        for g in gaps:
            if stts_entries and stts_entries[-1][1] == g:
                stts_entries[-1][0] += 1
            else:
                stts_entries.append([1, g])
        w, h = self.width, self.height
        disp_w, disp_h = (h, w) if self.turns & 1 else (w, h)

        stsd_entry = (
            b"jpeg"
            + bytes(6) + struct.pack(">H", 1)          # 예약, 데이터 참조 번호
            + struct.pack(">HH", 0, 0) + b"appl"       # 버전, 개정, 제조사
            + struct.pack(">II", 0, 512)               # 시간 품질, 공간 품질
            + struct.pack(">HH", w, h)
            + struct.pack(">II", 72 << 16, 72 << 16)   # 해상도 72dpi
            + struct.pack(">IH", 0, 1)                 # 데이터 크기, 장 수
            + bytes([12]) + b"Photo - JPEG".ljust(31, b"\0")
            + struct.pack(">hh", 24, -1)               # 색 깊이, 색 표 없음
        )
        stsd = _box(b"stsd", struct.pack(">II", 0, 1), struct.pack(">I", 4 + len(stsd_entry)) + stsd_entry)
        stts = _box(b"stts", struct.pack(">II", 0, len(stts_entries)), *(struct.pack(">II", c, g) for c, g in stts_entries))
        stsc = _box(b"stsc", struct.pack(">II", 0, 1), struct.pack(">III", 1, 1, 1))
        stsz = _box(b"stsz", struct.pack(">III", 0, 0, n), *(struct.pack(">I", s) for s in self.sizes))
        co64 = _box(b"co64", struct.pack(">II", 0, n), *(struct.pack(">Q", o) for o in self.offsets))
        stbl = _box(b"stbl", stsd, stts, stsc, stsz, co64)

        vmhd = _box(b"vmhd", struct.pack(">I", 1), struct.pack(">HHHH", 0, 0, 0, 0))
        dref = _box(b"dref", struct.pack(">II", 0, 1), _box(b"alis", struct.pack(">I", 1)))
        minf = _box(b"minf", vmhd, _box(b"dinf", dref), stbl)
        mdhd = _box(b"mdhd", struct.pack(">IIIIIHH", 0, 0, 0, TIMESCALE, duration, 0, 0))
        hdlr = _box(b"hdlr", struct.pack(">I", 0), b"mhlr", b"vide", bytes(12), b"\0")
        mdia = _box(b"mdia", mdhd, hdlr, minf)
        tkhd = _box(
            b"tkhd",
            struct.pack(">I", 3),                      # 버전 0, 켜짐 | 영상에 포함
            struct.pack(">IIII", 0, 0, 1, 0),          # 만든 시각, 고친 시각, 트랙 번호, 예약
            struct.pack(">I", duration),
            bytes(8),
            struct.pack(">hhhh", 0, 0, 0, 0),          # 층, 대체 그룹, 음량, 예약
            _matrix(self.turns, w, h),
            struct.pack(">II", w << 16, h << 16),
        )
        trak = _box(b"trak", tkhd, mdia)
        mvhd = _box(
            b"mvhd",
            struct.pack(">IIIII", 0, 0, 0, TIMESCALE, duration),
            struct.pack(">IH", 0x10000, 0x100),        # 재생 속도 1, 음량 1
            bytes(10),
            _matrix(0, disp_w, disp_h),
            bytes(24),
            struct.pack(">I", 2),                      # 다음 트랙 번호
        )
        return _box(b"moov", mvhd, trak)


def _jpeg_size(data: bytes) -> tuple[int, int]:
    """JPEG 머리(SOF)에서 가로·세로를 읽는다."""
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            break
        marker = data[i + 1]
        length = struct.unpack(">H", data[i + 2 : i + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", data[i + 5 : i + 9])
            return w, h
        i += 2 + length
    return 640, 480


class VideoReceiver:
    def __init__(self, cfg: dict):
        self.dir = Path(cfg.get("dir", "")).expanduser() if cfg.get("dir") else DEFAULT_DIR
        self.lock = threading.Lock()
        self.writer: MovWriter | None = None
        self.frame_no = -1
        self.frame = bytearray()
        self.got = 0
        self.lost = 0

    def on_event(self, event: dict) -> None:
        video = event.get("video") if isinstance(event, dict) else None
        if not isinstance(video, dict):
            return
        with self.lock:
            if "start" in video:
                self._finish()
                try:
                    path, f = open_new(self.dir, str(video["start"]), "video.mov", "videos")
                except OSError as e:
                    print(f"[video] 저장할 파일을 만들지 못했습니다: {e}")
                    return
                self.writer = MovWriter(path, int(video.get("turns", 0)), f)
                self.frame_no, self.got, self.lost = -1, 0, 0
                print(f"[video] 녹화 받는 중: {path.name}")
            elif "end" in video:
                dropped = int(video.get("dropped", 0))
                self._finish(dropped)

    def _finish(self, dropped: int = 0) -> None:
        if not self.writer:
            return
        path = self.writer.path
        frames = self.writer.close()
        self.writer = None
        if frames == 0:
            path.unlink(missing_ok=True)
            print("[video] 받은 장이 없어 파일을 만들지 않았습니다")
            return
        note = f", 기기에서 버린 장 {dropped}" if dropped else ""
        note += f", 덜 받은 장 {self.lost}" if self.lost else ""
        print(f"[video] 저장했습니다: {path} ({frames}장{note})")

    def on_chunk(self, no: int, offset: int, total: int, ms: int, data: bytes) -> None:
        with self.lock:
            if not self.writer or total > 4_000_000:
                return
            if no != self.frame_no:
                if self.frame_no >= 0 and self.got < len(self.frame):
                    self.lost += 1
                self.frame_no, self.frame, self.got = no, bytearray(total), 0
            if offset + len(data) > len(self.frame):
                return
            self.frame[offset : offset + len(data)] = data
            self.got += len(data)
            if self.got == len(self.frame):
                self.writer.add(bytes(self.frame), ms)
                self.got = len(self.frame) + 1  # 이 장은 끝났다 (중복 저장 방지)
