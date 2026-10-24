"""기기가 찍어 보낸 사진에서 QR·바코드를 읽는다 (photos.py 가 저장한 뒤 부른다).

2026-10-24 기기 카메라(OV5640, 2560x1920)로 시험해 정한 방법:
  - QR 은 오류 정정이 있어 그대로 / 국소 대비(CLAHE) / 윤곽 강조 중 하나에서 읽히면 인정한다.
  - 1D 바코드(EAN-13 등)는 사진이 조금만 흐려도 가는 막대가 뭉개지고, 검사 숫자까지 맞는 틀린 번호가 나온다.
    그래서 바코드 자리를 찾아(OpenCV) 잘라 펴고, 흐림 되돌리기(Richardson-Lucy, σ 1~2)·키우기·이진화 등
    여러 손질로 읽은 표를 모아, 가장 많이 나온 값이 3번 이상이고 2등의 2배 이상일 때만 인정한다.
    ("두 번 일치" 만으로는 8809381792717 대신 8209381798717 같은 값이 통과했다)
opencv-python-headless 와 zxing-cpp 가 없으면 아무것도 하지 않는다 (available False).
"""

from collections import Counter

try:
    import cv2
    import numpy as np
    import zxingcpp
    available = True
except ImportError:
    available = False

# 1D(막대) 바코드 종류 (이 zxing-cpp 판에서는 BarcodeFormat 에 `in LinearCodes` 를 쓸 수 없어 이름으로 가른다)
LINEAR = {"EAN8", "EAN13", "UPCA", "UPCE", "Code39", "Code93", "Code128", "ITF", "Codabar",
          "DataBar", "DataBarExpanded", "DataBarLimited", "DXFilmEdge"}
MIN_VOTES = 3  # 1D 값을 인정하는 최소 표
LEAD = 2  # 2등보다 이만큼 배 많아야 인정


def _blur_psf(sigma: float):
    k = cv2.getGaussianKernel(int(sigma * 3) * 2 + 1, sigma)
    return (k @ k.T).astype(np.float32)


def _deblur(gray, sigma: float, rounds: int = 20):
    """Richardson-Lucy 흐림 되돌리기 (초점이 조금 나간 사진의 막대 경계를 다시 세운다)"""
    psf = _blur_psf(sigma)
    flipped = psf[::-1, ::-1]
    img = gray.astype(np.float32) / 255 + 1e-3
    est = img.copy()
    for _ in range(rounds):
        ratio = img / (cv2.filter2D(est, -1, psf, borderType=cv2.BORDER_REFLECT) + 1e-6)
        est = np.clip(est * cv2.filter2D(ratio, -1, flipped, borderType=cv2.BORDER_REFLECT), 0, 1)
    return (est * 255).astype(np.uint8)


def _read(img) -> list:
    found = []
    for binarizer in (zxingcpp.Binarizer.LocalAverage, zxingcpp.Binarizer.GlobalHistogram):
        found += zxingcpp.read_barcodes(img, binarizer=binarizer)
    return found


def _whole_variants(gray):
    yield gray
    yield cv2.createCLAHE(clipLimit=3.0, tileGridSize=(8, 8)).apply(gray)
    yield cv2.addWeighted(gray, 2.5, cv2.GaussianBlur(gray, (0, 0), 2), -1.5, 0)


def _crops(gray, detector):
    """바코드 자리마다 둘레를 조금 넓혀 똑바로 편 그림 (긴 변이 가로).
    검출기가 주는 네 꼭짓점의 순서는 일정하지 않아, 순서대로 펴면 막대가 눕고 찌그러졌다 (2026-10-24).
    그래서 꼭짓점을 감싸는 회전된 최소 사각형으로 기울기와 길이를 구한다"""
    ok, quads = detector.detect(gray)
    if not ok or quads is None:
        return
    for quad in quads:
        (cx, cy), (w, h), angle = cv2.minAreaRect(quad.reshape(-1, 2).astype(np.float32))
        if w < h:
            w, h, angle = h, w, angle + 90
        if w < 10 or h < 10:
            continue
        w, h = int(w * 1.15), int(h * 1.15)  # 막대 양끝의 빈 여백까지
        turn = cv2.getRotationMatrix2D((cx, cy), angle, 1.0)
        # 자를 둘레만 돌린다 (큰 사진 전체를 돌리지 않게)
        turn[0, 2] += w / 2 - cx
        turn[1, 2] += h / 2 - cy
        yield cv2.warpAffine(gray, turn, (w, h), flags=cv2.INTER_CUBIC, borderValue=255)


def _crop_variants(crop):
    up = cv2.resize(crop, None, fx=3, fy=3, interpolation=cv2.INTER_CUBIC)
    yield up
    yield cv2.createCLAHE(clipLimit=3.0, tileGridSize=(4, 4)).apply(up)
    for sigma in (1.0, 1.5, 2.0):
        yield cv2.resize(_deblur(crop, sigma), None, fx=3, fy=3, interpolation=cv2.INTER_CUBIC)
    for block in (31, 61):
        yield cv2.adaptiveThreshold(up, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C, cv2.THRESH_BINARY, block, 5)


def _winner(votes: Counter) -> str | None:
    ranked = votes.most_common(2)
    if not ranked or ranked[0][1] < MIN_VOTES:
        return None
    if len(ranked) > 1 and ranked[0][1] < LEAD * ranked[1][1]:
        return None
    return ranked[0][0]


def scan(path) -> list[tuple[str, str]]:
    """사진 파일에서 읽은 것 [(종류, 글)]. 같은 것은 한 번만. QR 이 먼저."""
    if not available:
        return []
    return scan_gray(cv2.imread(str(path), cv2.IMREAD_GRAYSCALE))


def scan_bytes(data: bytes) -> list[tuple[str, str]]:
    """JPEG 바이트에서 (연속 스캔 사진은 파일로 남기지 않는다)"""
    if not available:
        return []
    return scan_gray(cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_GRAYSCALE))


def locate_bytes(data: bytes) -> list[int] | None:
    """스캔 1단계(넓은 장)에서 가장 큰 바코드·QR 자리 [x0, y0, x1, y1] (장 기준 0~1000). 없으면 None.
    기기가 센서 창을 이 자리로 옮기므로 회전 정보(EXIF)는 무시하고 센서가 찍은 그대로의 좌표를 쓴다"""
    if not available:
        return None
    gray = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_GRAYSCALE | cv2.IMREAD_IGNORE_ORIENTATION)
    if gray is None:
        return None
    h, w = gray.shape
    boxes = []
    ok, quads = cv2.barcode.BarcodeDetector().detect(gray)
    if ok and quads is not None:
        boxes += [q.reshape(-1, 2) for q in quads]
    ok, quad = cv2.QRCodeDetector().detect(gray)
    if ok and quad is not None:
        boxes.append(quad.reshape(-1, 2))
    if not boxes:
        return None
    pts = max(boxes, key=lambda q: cv2.contourArea(q.astype(np.float32)))
    x0, y0 = pts.min(axis=0)
    x1, y1 = pts.max(axis=0)
    clamp = lambda v, n: int(min(max(v / n, 0.0), 1.0) * 1000)
    return [clamp(x0, w), clamp(y0, h), clamp(x1, w), clamp(y1, h)]


def scan_gray(gray) -> list[tuple[str, str]]:
    if gray is None:
        return []
    codes: dict[str, str] = {}
    kinds: dict[str, str] = {}
    # 사진 전체에서는 QR 등 2D 만 받는다 (전체에서 바로 읽힌 1D 는 UPC-E 0074712000078 같은 틀린 값이 3번씩 나왔다)
    for img in _whole_variants(gray):
        for r in _read(img):
            if r.format.name not in LINEAR:
                codes.setdefault(r.text, r.format.name)
    found_1d = set()
    detector = cv2.barcode.BarcodeDetector()
    for crop in _crops(gray, detector):
        votes = Counter()
        for img in _crop_variants(crop):
            for im in (img, cv2.rotate(img, cv2.ROTATE_90_CLOCKWISE)):
                # 이진화는 기본 하나로: 표 기준(MIN_VOTES)을 이것으로 정했다 (둘로 읽으면 틀린 값도 표가 두 배)
                for r in zxingcpp.read_barcodes(im, formats=zxingcpp.BarcodeFormat.LinearCodes):
                    votes[r.text] += 1
                    kinds[r.text] = r.format.name
        win = _winner(votes)
        if win:
            found_1d.add(win)
    out = [(kind, text) for text, kind in codes.items()]
    out += [(kinds[text], text) for text in sorted(found_1d)]
    return out


def describe(found: list[tuple[str, str]]) -> str:
    """기기 알림 글 ("QR: ...", "바코드: ...") 한 줄씩, 길면 줄인다"""
    lines = []
    for kind, text in found:
        label = "QR" if kind in ("QRCode", "MicroQRCode", "RMQRCode") else "바코드"
        short = text if len(text) <= 40 else text[:37] + "..."  # 기기 글꼴은 ASCII·한글만 (… 없음)
        lines.append(f"{label}: {short}")
    return "\n".join(lines[:3])
