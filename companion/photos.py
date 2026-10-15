"""기기가 찍은 사진을 받아 PC 에 저장한다.

기기는 먼저 알림 {"photo": {"name", "size"}} 을 보내고, 이어서 사진 조각을 보낸다 (USB / 블루투스).
Wi-Fi 는 알림에 "url" 이 붙어 오고, transport 가 그 주소에서 사진을 통째로 받아 넘긴다.
저장 위치는 설정의 [photos] dir (기본: 사진 폴더 아래 "LLM Deck").
"""

import re
import threading
from pathlib import Path

DEFAULT_DIR = Path.home() / "Pictures" / "LLM Deck"


class PhotoReceiver:
    def __init__(self, cfg: dict):
        self.dir = Path(cfg.get("dir", "")).expanduser() if cfg.get("dir") else DEFAULT_DIR
        self.lock = threading.Lock()
        self.name = None
        self.buf = None
        self.got = 0

    def on_event(self, event: dict) -> None:
        photo = event.get("photo") if isinstance(event, dict) else None
        if not isinstance(photo, dict) or "url" in photo:
            return
        size = int(photo.get("size", 0))
        if not 0 < size <= 20_000_000:
            return
        with self.lock:
            if self.buf is not None and self.got < len(self.buf):
                print(f"[photo] {self.name} 받는 중 끊겨 버립니다 ({self.got}/{len(self.buf)})")
            self.name = photo.get("name", "photo.jpg")
            self.buf = bytearray(size)
            self.got = 0
        print(f"[photo] {self.name} 받는 중 ({size // 1024}KB)")

    def on_chunk(self, offset: int, data: bytes) -> None:
        with self.lock:
            if self.buf is None or offset + len(data) > len(self.buf):
                return
            self.buf[offset : offset + len(data)] = data
            self.got += len(data)
            if self.got < len(self.buf):
                return
            name, data_all = self.name, bytes(self.buf)
            self.buf = None
        self.save(name, data_all)

    def save(self, name: str, data: bytes) -> Path:
        # 기기가 보낸 이름은 파일 이름 글자만 남긴다 (폴더 밖으로 나가지 않게)
        safe = re.sub(r"[^A-Za-z0-9_.-]", "_", Path(name).name) or "photo.jpg"
        self.dir.mkdir(parents=True, exist_ok=True)
        path = self.dir / safe
        stem, n = path.stem, 1
        while path.exists():
            path = self.dir / f"{stem}_{n}{path.suffix}"
            n += 1
        path.write_bytes(data)
        print(f"[photo] 저장했습니다: {path} ({len(data) // 1024}KB)")
        return path
