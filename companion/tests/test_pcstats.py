from types import SimpleNamespace as NS

import pcstats
import pytest


def partition(mount, fstype="NTFS", opts="rw"):
    return NS(mountpoint=mount, device=mount, fstype=fstype, opts=opts)


@pytest.fixture
def windows(monkeypatch):
    monkeypatch.setattr(pcstats.sys, "platform", "win32")
    monkeypatch.setattr(pcstats, "_system_disk", lambda: "C:\\")


def test_all_local_drives_skip_inaccessible_and_remote(windows, monkeypatch):
    monkeypatch.setattr(pcstats.psutil, "disk_partitions", lambda all: [
        partition("c:\\"), partition("D:\\"), partition("E:\\"),
        partition("Z:\\", opts="rw,remote"), partition("F:\\", "CDFS"),
    ])
    calls = []

    def usage(mount):
        calls.append(mount)
        if mount == "E:\\":
            raise PermissionError("locked")
        return NS(total=1024**4, free=512 * 1024**3, percent=50)

    monkeypatch.setattr(pcstats.psutil, "disk_usage", usage)
    assert pcstats._disks() == [
        {"name": "C:", "pct": 50, "free": "512.0GB", "total": "1.0TB"},
        {"name": "D:", "pct": 50, "free": "512.0GB", "total": "1.0TB"},
    ]
    assert calls == ["C:\\", "D:\\", "E:\\"]


def test_empty_or_unavailable_drives_do_not_break_collection(windows, monkeypatch):
    monkeypatch.setattr(pcstats.psutil, "disk_partitions", lambda all: [partition("D:\\")])
    monkeypatch.setattr(pcstats.psutil, "disk_usage", lambda mount: NS(total=0, free=0, percent=0))
    assert pcstats._disks() == []


def test_macos_skips_internal_helper_volumes(monkeypatch):
    monkeypatch.setattr(pcstats.sys, "platform", "darwin")
    monkeypatch.setattr(pcstats, "_system_disk", lambda: "/System/Volumes/Data")
    monkeypatch.setattr(pcstats.psutil, "disk_partitions", lambda all: [
        partition("/", "apfs"), partition("/System/Volumes/VM", "apfs"),
        partition("/Volumes/Backup", "apfs"), partition("/Volumes/Share", "smbfs"),
    ])
    monkeypatch.setattr(pcstats.psutil, "disk_usage", lambda mount: NS(total=1024, free=512, percent=50))
    assert [d["name"] for d in pcstats._disks()] == ["시스템", "Backup"]


def test_collect_gpu_multi_disk_and_legacy_fields(monkeypatch):
    clock = [100.0]
    monkeypatch.setattr(pcstats.time, "time", lambda: clock[0])
    monkeypatch.setattr(pcstats.psutil, "cpu_percent", lambda interval: 24)
    monkeypatch.setattr(pcstats.psutil, "virtual_memory", lambda: NS(percent=42, used=4 * 1024**3, total=8 * 1024**3))
    monkeypatch.setattr(pcstats.psutil, "net_io_counters", lambda: NS(bytes_recv=clock[0] * 1024, bytes_sent=0))
    monkeypatch.setattr(pcstats.psutil, "sensors_battery", lambda: None)
    monkeypatch.setattr(pcstats, "WindowsGpu", lambda: NS(percent=lambda now: 67))
    disks = [{"name": "C:", "pct": 50, "free": "100GB", "total": "200GB"},
             {"name": "D:", "pct": 75, "free": "250GB", "total": "1TB"}]
    monkeypatch.setattr(pcstats, "_disks", lambda: disks)
    stats = pcstats.PcStats()
    assert not stats.due()
    stats.on_event({"pc_watch": True})
    assert stats.due()
    clock[0] += 3
    data = stats.collect()
    assert data["gpu"] == 67 and data["disks"] == disks
    assert data["disk"] == 50 and data["disk_text"] == "100GB 남음"
    assert data["down"] == "1KB/s"
    assert not stats.due()
    monkeypatch.setattr(pcstats, "_disks", lambda: [])
    stats.gpu.percent = lambda now: None
    data = stats.collect()
    assert data["disks"] == [] and data["gpu"] is None and data["disk_text"] == "-"
