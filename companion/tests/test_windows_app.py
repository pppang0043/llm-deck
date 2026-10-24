import importlib.machinery
import importlib.util
import struct
from pathlib import Path

PATH = Path(__file__).resolve().parents[1] / "windows" / "app.pyw"


def load():
    loader = importlib.machinery.SourceFileLoader("windows_app", str(PATH))
    module = importlib.util.module_from_spec(importlib.util.spec_from_loader("windows_app", loader))
    loader.exec_module(module)
    return module


def rgb(app, v: int) -> bytes:
    return bytes((app.R5[v >> 11], app.G6[(v >> 5) & 63], app.R5[v & 31]))


def test_unpack_literal_and_repeat_runs():
    app = load()
    red, green, white = 0xF800, 0x07E0, 0xFFFF
    # 두 점 그대로 (머리 1), 흰 점 세 번 (머리 129)
    src = bytes([1]) + struct.pack("<HH", red, green) + bytes([129]) + struct.pack("<H", white)
    assert app.unpack(src, 5) == rgb(app, red) + rgb(app, green) + rgb(app, white) * 3
    assert rgb(app, white) == b"\xff\xff\xff"


def test_unpack_stops_at_short_input_and_pixel_count():
    app = load()
    # 점이 모자라면 나머지는 검게, 넘치면 자른다
    assert app.unpack(bytes([0]) + struct.pack("<H", 0xFFFF), 2) == b"\xff\xff\xff\x00\x00\x00"
    assert app.unpack(bytes([200]) + struct.pack("<H", 0xFFFF), 3) == b"\xff\xff\xff" * 3
    assert app.unpack(bytes([5, 0xFF]), 2) == b"\x00" * 6


def test_mirror_origin_keeps_bottom_right_corner_on_screen():
    app = load()
    area = (0, 0, 1920, 1040)  # 작업 표시줄을 뺀 메인 모니터
    # 오른쪽 아래 모서리를 그 자리에 둔다
    assert app.mirror_origin((336, 279), (1908, 1028), area) == (1572, 749)
    # 창이 커져 화면 밖으로 나가면 안으로 당긴다
    assert app.mirror_origin((976, 759), (500, 300), area) == (0, 0)
    assert app.mirror_origin((336, 279), (2500, 1500), area) == (1584, 761)
    # 왼쪽 위 모니터 (음수 좌표)
    assert app.mirror_origin((336, 279), (-100, -50), (-1920, -1080, 0, 0)) == (-436, -329)
