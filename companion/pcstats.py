"""기기의 "PC 상태" 화면에 보낼 이 PC 의 CPU, GPU, 메모리, 드라이브별 디스크, 네트워크, 배터리.

기기는 그 화면이 보이는 동안 5초마다 {"pc_watch": true} 를 보낸다. 그 뒤 15초 동안 3초마다 보낸다
(화면을 안 볼 때는 보내지 않아 블루투스를 아낀다).
"""

import functools
import ntpath
import os
import plistlib
import re
import subprocess
import sys
import time

import psutil

from wingpu import WindowsGpu

WATCH_SECONDS = 15
SEND_EVERY = 3


def _size(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.0f}{unit}" if unit in ("B", "KB") else f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}TB"


def _system_disk() -> str:
    if os.name == "nt":
        return os.environ.get("SystemDrive", "C:") + "\\"
    # macOS 는 "/" 가 읽기 전용 시스템 칸이라, 사용자 파일이 있는 칸을 본다
    return "/System/Volumes/Data" if os.path.isdir("/System/Volumes/Data") else "/"


@functools.lru_cache(maxsize=64)
def _mac_disk_image(mount: str) -> bool:
    """Mac 에서 .dmg 같은 디스크 이미지인지. 실제 디스크(내장, USB, 외장)만 보여 주려고 쓴다."""
    try:
        out = subprocess.run(["/usr/sbin/diskutil", "info", "-plist", mount], capture_output=True, timeout=3).stdout
        return plistlib.loads(out).get("BusProtocol") == "Disk Image"
    except (OSError, subprocess.SubprocessError, plistlib.InvalidFileException, AttributeError):
        return False


def _disks() -> list[dict]:
    """시스템 디스크와 접근 가능한 로컬 드라이브. 네트워크/광학 드라이브, Mac 의 디스크 이미지(.dmg)는 제외."""
    system = _system_disk()
    candidates = [system]
    try:
        candidates += [p.mountpoint for p in psutil.disk_partitions(all=False)
                       if p.fstype and p.fstype.lower() not in ("cdfs", "udf", "smbfs", "cifs", "nfs", "nfs4")
                       and "remote" not in p.opts.lower() and "cdrom" not in p.opts.lower()]
    except OSError:
        pass
    seen: set[str] = set()
    disks = []
    paths = ntpath if sys.platform == "win32" else os.path
    for mount in candidates:
        if sys.platform == "darwin" and mount != system and not mount.startswith("/Volumes/"):
            continue  # APFS 의 시스템 보조 볼륨은 중복 표시하지 않는다
        if sys.platform == "darwin" and mount != system and _mac_disk_image(mount):
            continue  # 프로그램 설치용 .dmg 등은 하드웨어 디스크가 아니다
        key = paths.normcase(paths.normpath(mount))
        if key in seen:
            continue
        seen.add(key)
        try:
            usage = psutil.disk_usage(mount)
        except OSError:
            continue  # 비어 있는 카드 리더, 권한 없는 드라이브 등
        if usage.total <= 0:
            continue
        name = mount.rstrip("/\\") or "/"
        if sys.platform != "win32":
            name = "시스템" if mount == system else os.path.basename(name) or "/"
        disks.append({"name": name[:24], "pct": round(usage.percent),
                      "free": _size(usage.free), "total": _size(usage.total)})
        if len(disks) == 26:
            break
    return disks  # Windows 드라이브 문자 수, 기기 표시 한도와 같다


class MacGpu:
    """Mac GPU 사용률. ioreg 의 IOAccelerator "Device Utilization %" (관리자 권한 필요 없음). 못 읽으면 None."""

    def percent(self, now: float) -> int | None:
        try:
            out = subprocess.run(["/usr/sbin/ioreg", "-r", "-d", "1", "-c", "IOAccelerator"],
                                 capture_output=True, text=True, timeout=2).stdout
        except (OSError, subprocess.SubprocessError):
            return None
        values = [int(v) for v in re.findall(r'"Device Utilization %"=(\d+)', out)]
        return min(100, max(values)) if values else None


class PcStats:
    def __init__(self) -> None:
        self.watch_until = 0.0
        self.sent_at = 0.0
        psutil.cpu_percent(interval=None)  # 첫 호출은 0 이 나오므로 미리 한 번
        self.gpu = MacGpu() if sys.platform == "darwin" else WindowsGpu()
        self._net = psutil.net_io_counters()
        self._net_at = time.time()

    def on_event(self, event: dict) -> None:
        if event.get("pc_watch") is True:
            self.watch_until = time.time() + WATCH_SECONDS

    def due(self) -> bool:
        now = time.time()
        return now < self.watch_until and now - self.sent_at >= SEND_EVERY

    def collect(self) -> dict:
        self.sent_at = time.time()
        mem = psutil.virtual_memory()
        disks = _disks()
        net = psutil.net_io_counters()
        now = time.time()
        span = max(now - self._net_at, 0.1)
        down = (net.bytes_recv - self._net.bytes_recv) / span
        up = (net.bytes_sent - self._net.bytes_sent) / span
        self._net, self._net_at = net, now
        stats = {
            "cpu": round(psutil.cpu_percent(interval=None)),
            "ram": round(mem.percent),
            "ram_text": f"{_size(mem.used)} / {_size(mem.total)}",
            # 예전 펌웨어도 시스템 디스크 한 개는 계속 표시한다.
            "disk": disks[0]["pct"] if disks else 0,
            "disk_text": f"{disks[0]['free']} 남음" if disks else "-",
            "disks": disks,
            "gpu": self.gpu.percent(time.monotonic()),
            "down": _size(down) + "/s",
            "up": _size(up) + "/s",
        }
        battery = psutil.sensors_battery() if hasattr(psutil, "sensors_battery") else None
        if battery is not None:
            stats["battery"] = round(battery.percent)
            stats["plugged"] = bool(battery.power_plugged)
        return stats
