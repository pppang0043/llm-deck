import base64
import time
import zipfile

import sd_package
import sdpkg


def test_stage_folder(tmp_path):
    src = tmp_path / "src"
    src.mkdir()
    zips = []
    for name in ("llm-deck-companion-windows.zip", "llm-deck-companion-mac.zip"):
        with zipfile.ZipFile(src / name, "w") as z:
            z.writestr("a.txt", name)
        zips.append(src / name)

    folder = sd_package.stage(tmp_path / "stage", zips, "abc1234")

    assert sorted(p.name for p in folder.iterdir()) == [
        "README.txt", "llm-deck-companion-mac.zip", "llm-deck-companion-windows.zip", "version.txt"]
    readme = (folder / "README.txt").read_bytes()
    assert readme.startswith("﻿".encode()) and b"\r\n" in readme and b"abc1234" in readme
    assert (folder / "version.txt").read_text().startswith("abc1234 ")


class FakeDevice:
    """firmware/src/helper_pkg.cpp 처럼 name.part 에 이어 쓰고 done 이면 이름을 바꾼다"""
    name = "USB"

    def __init__(self, pkg, card_ver=""):
        self.pkg, self.files, self.parts, self.ver = pkg, {}, {}, card_ver
        self.sent = []

    def send(self, message):
        p = message["pkg"]
        self.sent.append(p.get("name"))
        if p.get("ask"):
            reply = {"card": True, "ver": self.ver}
        else:
            part = self.parts.setdefault(p["name"], b"") if p["off"] else b""
            if p["off"] == len(part):
                part += base64.b64decode(p["data"])
                self.parts[p["name"]] = part
                if p["done"]:
                    self.files[p["name"]] = self.parts.pop(p["name"])
                    if p["name"] == "version.txt":
                        self.ver = part.decode().strip()
            reply = {"name": p["name"], "off": len(part)}
        self.pkg.on_event({"pkg": reply})


def wait(pkg):
    for _ in range(200):
        if not pkg.busy:
            return
        time.sleep(0.01)
    raise AssertionError("still busy")


def test_sends_stage_to_card_version_last(tmp_path):
    (tmp_path / "big.zip").write_bytes(bytes(range(256)) * 60)  # 여러 조각
    (tmp_path / "README.txt").write_text("hi")
    (tmp_path / "version.txt").write_text("v1 aa\n")
    pkg = sdpkg.SdPackage(tmp_path)
    dev = FakeDevice(pkg)

    pkg.poll(dev)
    wait(pkg)

    assert dev.files["big.zip"] == (tmp_path / "big.zip").read_bytes()
    assert dev.files["README.txt"] == b"hi"
    assert dev.ver == "v1 aa" and dev.sent[-1] == "version.txt"

    # 같은 버전이면 다시 묻지도 않는다 (1분 뒤에도)
    dev.sent.clear()
    pkg.checked = 0
    pkg.poll(dev)
    wait(pkg)
    assert dev.sent == []


def test_skips_when_card_has_same_version(tmp_path):
    (tmp_path / "a.zip").write_bytes(b"x")
    (tmp_path / "version.txt").write_text("v1 aa\n")
    pkg = sdpkg.SdPackage(tmp_path)
    dev = FakeDevice(pkg, card_ver="v1 aa")

    pkg.poll(dev)
    wait(pkg)

    assert dev.sent == [None] and dev.files == {}


def test_nothing_without_stage_or_usb(tmp_path):
    pkg = sdpkg.SdPackage(tmp_path / "none")
    dev = FakeDevice(pkg)
    pkg.poll(dev)
    assert not pkg.busy and dev.sent == []
    (tmp_path / "version.txt").write_text("v1\n")
    dev.name = "wifi"
    sdpkg.SdPackage(tmp_path).poll(dev)
    assert dev.sent == []
