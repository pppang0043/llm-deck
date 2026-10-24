import threading

import pytest

import photos
import scanner

pytestmark = pytest.mark.skipif(not scanner.available, reason="opencv / zxing-cpp 없음")

if scanner.available:
    import cv2
    import numpy as np
    import zxingcpp


def photo(tmp_path, text, fmt, scale, blur=0.0, name="code.jpg"):
    """흰 바탕 가운데에 코드를 그린 사진 (기기 사진처럼 조금 흐리게, JPEG 로)"""
    code = np.array(zxingcpp.write_barcode_to_image(zxingcpp.create_barcode(text, fmt), scale=scale))
    if code.shape[0] < 40:  # 1D 는 막대를 세로로 늘인다
        code = cv2.resize(code, (code.shape[1], 120), interpolation=cv2.INTER_NEAREST)
    img = np.full((1200, 1600), 235, np.uint8)
    y, x = (1200 - code.shape[0]) // 2, (1600 - code.shape[1]) // 2
    img[y:y + code.shape[0], x:x + code.shape[1]] = code
    if blur:
        img = cv2.GaussianBlur(img, (0, 0), blur)
    path = tmp_path / name
    cv2.imwrite(str(path), img, [cv2.IMWRITE_JPEG_QUALITY, 85])
    return path


def test_reads_qr(tmp_path):
    assert scanner.scan(photo(tmp_path, "LLM-DECK-TEST", zxingcpp.BarcodeFormat.QRCode, 8)) == [
        ("QRCode", "LLM-DECK-TEST")]


def test_reads_blurred_ean13(tmp_path):
    # 막대 하나 4화소, 막대 폭의 절반쯤 흐림: 사진 전체로는 안 읽혀도 잘라 펴고 흐림을 되돌려 읽는다
    path = photo(tmp_path, "880938179271", zxingcpp.BarcodeFormat.EAN13, 4, blur=2.0)
    assert scanner.scan(path) == [("EAN13", "8809381792717")]


def test_nothing_in_plain_photo(tmp_path):
    rng = np.random.default_rng(1)
    path = tmp_path / "noise.jpg"
    cv2.imwrite(str(path), rng.integers(0, 255, (1200, 1600), dtype=np.uint8))
    assert scanner.scan(path) == []
    assert scanner.scan(tmp_path / "missing.jpg") == []


def test_vote_needs_clear_winner():
    from collections import Counter
    assert scanner._winner(Counter({"8809381792717": 8, "8209381798717": 2})) == "8809381792717"
    assert scanner._winner(Counter({"0074712000078": 2})) is None  # 표가 모자람
    assert scanner._winner(Counter({"a": 4, "b": 3})) is None  # 2등과 비슷하면 믿지 않는다


def test_scan_frame_is_not_saved_and_reports(tmp_path, monkeypatch):
    # 연속 스캔 사진(SCAN_*)은 사진 폴더에 남기지 않고, 읽으면 기기에 {"scan": 글} 로 알린다
    import orca
    monkeypatch.setattr(orca, "_copy", lambda t: True)
    receiver = photos.PhotoReceiver({"dir": str(tmp_path / "pics")}, tmp_path / "vids")
    data = photo(tmp_path, "880938179271", zxingcpp.BarcodeFormat.EAN13, 4, blur=1.5).read_bytes()
    assert receiver.save("SCAN_1.jpg", data) is None
    for _ in range(300):
        if receiver.pending_scan:
            break
        threading.Event().wait(0.05)
    assert receiver.pending_scan == "바코드: 8809381792717\n클립보드에 넣었습니다"
    assert not (tmp_path / "pics").exists() or not any((tmp_path / "pics").iterdir())


def test_locate_returns_box_in_sensor_coordinates(tmp_path):
    # 너무 흐려 읽히지는 않아도 자리는 찾는다: 사진 가운데의 바코드 -> 가운데쯤 상자 (0~1000)
    path = photo(tmp_path, "880938179271", zxingcpp.BarcodeFormat.EAN13, 4, blur=4.0)
    box = scanner.locate_bytes(path.read_bytes())
    assert box is not None
    x0, y0, x1, y1 = box
    assert 300 < (x0 + x1) / 2 < 700 and 300 < (y0 + y1) / 2 < 700 and x1 - x0 > 50


def test_locate_frame_reports_roi_or_none(tmp_path):
    receiver = photos.PhotoReceiver({"dir": str(tmp_path / "pics")}, tmp_path / "vids")
    rng = np.random.default_rng(2)
    empty = tmp_path / "empty.jpg"
    cv2.imwrite(str(empty), np.full((768, 1024), 200, np.uint8) + rng.integers(0, 5, (768, 1024), dtype=np.uint8))
    assert receiver.save("LOC_1.jpg", empty.read_bytes()) is None
    for _ in range(200):
        if receiver.pending_roi is not None:
            break
        threading.Event().wait(0.05)
    assert receiver.pending_roi == []  # 못 찾음: 기기가 바로 다시 넓게 찍는다


def test_describe_labels_and_shortens():
    text = scanner.describe([("QRCode", "x" * 60), ("EAN13", "8809381792717")])
    assert text.splitlines() == ["QR: " + "x" * 37 + "...", "바코드: 8809381792717"]


def test_saved_photo_is_scanned_and_toasted(tmp_path, monkeypatch):
    import orca
    copied = []
    monkeypatch.setattr(orca, "_copy", lambda t: copied.append(t) or True)
    done = threading.Event()
    real_describe = scanner.describe
    monkeypatch.setattr(scanner, "describe", lambda found: (done.set(), real_describe(found))[1])
    receiver = photos.PhotoReceiver({"dir": str(tmp_path / "pics")}, tmp_path / "vids")
    data = photo(tmp_path, "LLM-DECK-TEST", zxingcpp.BarcodeFormat.QRCode, 8).read_bytes()
    receiver.save("IMG_1.jpg", data)
    assert done.wait(10)
    for _ in range(50):  # describe 뒤에 알림 글을 넣는다
        if receiver.pending_toast:
            break
        threading.Event().wait(0.02)
    assert copied == ["LLM-DECK-TEST"]
    assert receiver.pending_toast == "QR: LLM-DECK-TEST\n클립보드에 넣었습니다"
