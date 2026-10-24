"""기기가 찍은 사진을 받아 PC 에 저장한다.

기기는 먼저 알림 {"photo": {"name", "size"}} 을 보내고, 이어서 사진 조각을 보낸다 (USB / 블루투스).
Wi-Fi 는 알림에 "url" 이 붙어 오고, transport 가 그 주소에서 사진을 통째로 받아 넘긴다.
저장 위치는 설정의 [photos] dir (기본: 사진 폴더 아래 "LLM Deck").
갤러리에서 다시 보낸 동영상(.mov)도 같은 길로 오고, 동영상 폴더에 저장한다 (받는 대로 임시 파일에 쓴다).
이미 같은 이름·크기·내용의 파일이 있으면 새로 만들지 않고 기기에 "PC에 이미 있습니다" 를 알린다.
이름은 같은데 내용이 다르면 이름 뒤에 _1, _2 ... 를 붙여 저장한다.
"""

import hashlib
import os
import re
import tempfile
import threading
from pathlib import Path

import scanner

DEFAULT_DIR = Path.home() / "Pictures" / "LLM Deck"
# 원래 폴더에 쓸 수 없을 때 대신 쓰는 곳. Windows 보안의 "제어된 폴더 액세스"가 켜져 있으면
# 처음 보는 프로그램이 사진·비디오 폴더에 쓰지 못하게 막는다.
FALLBACK_ROOT = Path.home() / ".llm-deck"


def unique_path(folder: Path, name: str, default: str) -> Path:
    """folder 안에 겹치지 않는 파일 경로. 기기가 보낸 이름은 파일 이름 글자만 남긴다 (폴더 밖으로 나가지 않게)."""
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", Path(name).name) or default
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / safe
    stem, n = path.stem, 1
    while path.exists():
        path = folder / f"{stem}_{n}{path.suffix}"
        n += 1
    return path


def open_new(folder: Path, name: str, default: str, fallback: str):
    """새 파일을 열어 (경로, 파일) 을 돌려준다. folder 에 못 쓰면 ~/.llm-deck/<fallback> 에 쓴다."""
    try:
        path = unique_path(folder, name, default)
        return path, path.open("xb")
    except OSError as e:
        other = FALLBACK_ROOT / fallback
        print(f"[save] {folder} 에 쓸 수 없어 {other} 에 저장합니다 ({e})")
        print("[save] Windows 라면 Windows 보안 > 랜섬웨어 방지 > 제어된 폴더 액세스에서 python.exe 를 허용하세요.")
        path = unique_path(other, name, default)
        return path, path.open("xb")


# 사진은 메모리에 모아 저장한다. 동영상(.mov)은 클 수 있어 받는 대로 임시 파일에 쓴다 (FAT32 카드라 4GB 미만).
PHOTO_MAX = 20_000_000
VIDEO_MAX = 2_000_000_000


def _sha256(path: Path) -> bytes:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.digest()


def find_same(folders: list[Path], name: str, default: str, size: int, digest) -> Path | None:
    """folders 에 이미 같은 파일(같은 이름 또는 그 이름_1, _2 ...; 크기와 내용이 같음)이 있으면 그 경로.
    digest 는 받은 내용의 sha256 을 돌려주는 함수 (크기가 같은 파일이 있을 때만 부른다)."""
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", Path(name).name) or default
    stem, suffix = Path(safe).stem, Path(safe).suffix
    want = None
    for folder in folders:
        path, n = folder / safe, 1
        while path.exists():
            try:
                if path.stat().st_size == size:
                    want = want or digest()
                    if _sha256(path) == want:
                        return path
            except OSError:
                pass
            path = folder / f"{stem}_{n}{suffix}"
            n += 1
    return None


class PhotoReceiver:
    def __init__(self, cfg: dict, video_dir: Path | None = None, scan_cfg: dict | None = None):
        self.dir = Path(cfg.get("dir", "")).expanduser() if cfg.get("dir") else DEFAULT_DIR
        self.video_dir = video_dir or self.dir
        self.lock = threading.Lock()
        self.name = None
        self.size = 0
        self.buf = None  # 사진: bytearray
        self.tmp = None  # 동영상: 받는 중인 임시 파일 (열린 파일)
        self.got = 0
        self.pending_toast: str | None = None  # main.py 가 기기 알림으로 보낸다
        self.pending_scan: str | None = None  # 연속 스캔 결과 {"scan": 글}: 기기가 보이고 스캔을 멈춘다
        self.pending_roi: list[int] | None = None  # 스캔 1단계 장의 바코드 자리 {"scan_roi": [...]} ([] 면 못 찾음)
        # config.toml [scan] type: 스캔으로 읽은 값을 커서 자리에 입력하고 Enter (바코드 스캐너처럼)
        self.type_scan = bool((scan_cfg or {}).get("type", False))

    def _folders(self, name: str) -> tuple[Path, str, str]:
        if name.lower().endswith(".mov"):
            return self.video_dir, "video.mov", "videos"
        return self.dir, "photo.jpg", "photos"

    def _drop(self) -> None:
        """받던 것을 버린다 (lock 을 잡고 부른다)."""
        if self.tmp is not None:
            try:
                self.tmp.close()
                Path(self.tmp.name).unlink(missing_ok=True)
            except OSError:
                pass
        self.tmp = None
        self.buf = None

    def _open_tmp(self, name: str):
        folder, _, fallback = self._folders(name)
        for where in (folder, FALLBACK_ROOT / fallback):
            try:
                where.mkdir(parents=True, exist_ok=True)
                return tempfile.NamedTemporaryFile(dir=where, prefix=".receiving-", suffix=".part", delete=False)
            except OSError as e:
                print(f"[photo] {where} 에 임시 파일을 만들 수 없습니다 ({e})")
        return None

    def on_event(self, event: dict) -> None:
        photo = event.get("photo") if isinstance(event, dict) else None
        if not isinstance(photo, dict) or "url" in photo:
            return
        try:
            size = int(photo.get("size", 0))
        except (TypeError, ValueError):
            return
        name = str(photo.get("name", "photo.jpg"))
        video = name.lower().endswith(".mov")
        if not 0 < size <= (VIDEO_MAX if video else PHOTO_MAX):
            print(f"[photo] {name} 은 너무 커서 받지 않습니다 ({size // 1024}KB)")
            return
        with self.lock:
            if (self.buf is not None or self.tmp is not None) and self.got < self.size:
                print(f"[photo] {self.name} 받는 중 끊겨 버립니다 ({self.got}/{self.size})")
            self._drop()
            self.name, self.size, self.got = name, size, 0
            if video:
                self.tmp = self._open_tmp(name)
                if self.tmp is None:
                    return
            else:
                self.buf = bytearray(size)
        print(f"[photo] {name} 받는 중 ({size // 1024}KB)")

    def on_chunk(self, offset: int, data: bytes) -> None:
        with self.lock:
            if (self.buf is None and self.tmp is None) or offset < 0 or offset + len(data) > self.size:
                return
            try:
                if self.tmp is not None:
                    if self.tmp.tell() != offset:
                        self.tmp.seek(offset)
                    self.tmp.write(data)
                else:
                    self.buf[offset : offset + len(data)] = data
            except OSError as e:
                print(f"[photo] {self.name} 을 쓰지 못해 버립니다: {e}")
                self._drop()
                return
            self.got += len(data)
            if self.got < self.size:
                return
            name, buf, tmp = self.name, self.buf, self.tmp
            self.buf = self.tmp = None
        if tmp is not None:
            self.save_file(name, tmp)
        else:
            self.save(name, bytes(buf))

    def _same(self, name: str, size: int, digest) -> Path | None:
        folder, default, fallback = self._folders(name)
        try:
            return find_same([folder, FALLBACK_ROOT / fallback], name, default, size, digest)
        except OSError:
            return None

    def _skip(self, name: str, same: Path) -> None:
        print(f"[photo] 이미 있습니다: {same} (같은 파일이라 저장하지 않습니다)")
        self.pending_toast = f"PC에 이미 있습니다\n{same.name}"

    def save(self, name: str, data: bytes) -> Path | None:
        if name.startswith("SCAN_"):  # 기기 카메라의 연속 스캔 사진: 저장하지 않고 읽기만 한다
            self._scan_frame(name, data)
            return None
        if name.startswith("LOC_"):  # 스캔 1단계(넓은 장): 읽히면 끝, 아니면 바코드 자리를 알려 준다
            self._scan_frame(name, data, locate=True)
            return None
        same = self._same(name, len(data), lambda: hashlib.sha256(data).digest())
        if same:
            self._skip(name, same)
            return same
        folder, default, fallback = self._folders(name)
        try:
            path, f = open_new(folder, name, default, fallback)
            with f:
                f.write(data)
        except OSError as e:
            # 받는 쪽(연결 스레드)이 멈추지 않도록 여기서 끝낸다
            print(f"[photo] 저장하지 못했습니다: {e}")
            return None
        print(f"[photo] 저장했습니다: {path} ({len(data) // 1024}KB)")
        self._scan(path)
        return path

    def _scan_frame(self, name: str, data: bytes, locate: bool = False) -> None:
        if not scanner.available:
            self.pending_scan = "PC 도우미에 판독기가 없습니다\n(opencv, zxing-cpp)"
            return

        keep = os.environ.get("LLMDECK_SCAN_DIR")  # 진단용: 연속 스캔 사진을 이 폴더에 남긴다
        if keep:
            try:
                Path(keep).mkdir(parents=True, exist_ok=True)
                (Path(keep) / name).write_bytes(data)
            except OSError:
                pass

        def run():
            try:
                found = scanner.scan_bytes(data)
            except Exception as e:
                print(f"[scan] {name} 읽지 못했습니다: {e}")
                return
            if not found:
                if locate:
                    roi = scanner.locate_bytes(data)
                    print(f"[scan] {name}: 자리 {roi}")
                    self.pending_roi = roi or []
                else:
                    print(f"[scan] {name}: 못 찾음")
                return
            from orca import _copy

            copied = _copy("\n".join(text for _, text in found))
            typed = None
            if self.type_scan:
                import typer

                error = typer._type(found[0][1], enter=True)
                typed = "커서 자리에 입력했습니다" if not error else f"입력하지 못했습니다: {error}"
            print(f"[scan] {name}: {found}{' (클립보드에 넣음)' if copied else ''}{f' ({typed})' if typed else ''}")
            note = typed or ("클립보드에 넣었습니다" if copied else "")
            self.pending_scan = scanner.describe(found) + (f"\n{note}" if note else "")

        threading.Thread(target=run, daemon=True).start()

    def _scan(self, path: Path) -> None:
        """찍은 사진에 QR·바코드가 있으면 기기에 알리고 클립보드에 넣는다 (받는 쪽이 멈추지 않게 따로)"""
        if not scanner.available or path.suffix.lower() == ".mov":
            return

        def run():
            try:
                found = scanner.scan(path)
            except Exception as e:  # 판독이 실패해도 사진 받기에는 영향이 없게
                print(f"[scan] {path.name} 읽지 못했습니다: {e}")
                return
            if not found:
                return
            from orca import _copy  # 받아쓰기와 같은 클립보드 넣기 (Mac pbcopy, Windows clip)

            copied = _copy("\n".join(text for _, text in found))
            print(f"[scan] {path.name}: {found}{' (클립보드에 넣음)' if copied else ''}")
            self.pending_toast = scanner.describe(found) + ("\n클립보드에 넣었습니다" if copied else "")

        threading.Thread(target=run, daemon=True).start()

    def save_file(self, name: str, tmp) -> Path | None:
        """다 받은 임시 파일을 이름을 붙여 옮긴다. 같은 파일이 이미 있으면 지운다."""
        part = Path(tmp.name)
        try:
            tmp.close()
            size = part.stat().st_size
            same = self._same(name, size, lambda: _sha256(part))
            if same:
                part.unlink(missing_ok=True)
                self._skip(name, same)
                return same
            # 임시 파일과 같은 폴더(같은 디스크)라 바로 옮겨진다
            path = unique_path(part.parent, name, self._folders(name)[1])
            os.replace(part, path)
        except OSError as e:
            print(f"[photo] 저장하지 못했습니다: {e}")
            try:
                part.unlink(missing_ok=True)
            except OSError:
                pass
            return None
        print(f"[photo] 저장했습니다: {path} ({size // 1024}KB)")
        return path
