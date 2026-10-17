"""기기의 "PC 상태" 화면에 보낼 이 PC 의 CPU, 메모리, 디스크, 네트워크, 배터리.

기기는 그 화면이 보이는 동안 5초마다 {"pc_watch": true} 를 보낸다. 그 뒤 15초 동안 3초마다 보낸다
(화면을 안 볼 때는 보내지 않아 블루투스를 아낀다).
"""

import os
import time

import psutil

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


class PcStats:
    def __init__(self) -> None:
        self.watch_until = 0.0
        self.sent_at = 0.0
        psutil.cpu_percent(interval=None)  # 첫 호출은 0 이 나오므로 미리 한 번
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
        disk = psutil.disk_usage(_system_disk())
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
            "disk": round(disk.percent),
            "disk_text": f"{_size(disk.free)} 남음",
            "down": _size(down) + "/s",
            "up": _size(up) + "/s",
        }
        battery = psutil.sensors_battery() if hasattr(psutil, "sensors_battery") else None
        if battery is not None:
            stats["battery"] = round(battery.percent)
            stats["plugged"] = bool(battery.power_plugged)
        return stats
