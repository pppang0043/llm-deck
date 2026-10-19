"""기기가 USB 와 Wi-Fi(TCP)로 보내는 묶음(프레임) 형식. 펌웨어의 usb_link.h 와 맞춘다.

    [0xA5 0x5A][종류 1바이트][길이 2바이트][내용]
"""

import json
import struct
import sys
import time
import traceback

MAGIC = b"\xa5\x5a"
FRAME_AUDIO = 1
FRAME_EVENT = 2
FRAME_PHOTO = 3
FRAME_VIDEO = 4
FRAME_MIRROR = 5  # 미러링 화면 조각 (mirror.py)
# 미러링 화면 조각을 받을 곳 (main.py 가 mirror.Mirror.on_frame 을 넣는다)
mirror_sink = None
MAX_FRAME = 2048
VIDEO_HEAD = struct.Struct("<IIII")  # 장 번호, 위치, 장 크기, 시각(ms)
# 오류 종류 -> [마지막으로 적은 시각, 그 뒤로 적지 않은 횟수]
_reported: dict[tuple, list] = {}


def report(where: str) -> None:
    """받은 것을 처리하다 난 오류를 기록만 하고 넘어간다 (except 안에서 부른다).

    받는 스레드가 오류로 끝나면 그 연결로는 더 받지 못한다 (USB 는 조용해진 것으로 보고 기기를 다시 켜 버린다).
    같은 오류가 묶음마다 되풀이될 수 있어, 자세한 내용은 처음 한 번만 남기고 그 뒤로는 1분에 한 줄만 적는다.
    """
    e = sys.exc_info()[1]
    tb = traceback.extract_tb(e.__traceback__) if e else []
    key = (where, type(e).__name__, tb[-1].lineno if tb else 0)
    now = time.monotonic()
    seen = _reported.get(key)
    if seen is None:
        _reported[key] = [now, 0]
        print(f"[{where}] 받은 것을 처리하지 못했습니다 (계속 받습니다):")
        traceback.print_exc()
    elif now - seen[0] >= 60:
        print(f"[{where}] 같은 오류가 또 났습니다 ({seen[1] + 1}번): {type(e).__name__}: {e}")
        _reported[key] = [now, 0]
    else:
        seen[1] += 1


def dispatch(transport, kind: int, body: bytes) -> None:
    """프레임 하나를 transport 의 on_audio / on_event / on_photo / on_video 로 넘긴다. 처리 중 오류는 기록만 한다."""
    try:
        _dispatch(transport, kind, body)
    except Exception:
        report(getattr(transport, "name", "device"))


def _dispatch(transport, kind: int, body: bytes) -> None:
    if kind == FRAME_AUDIO:
        (seq,) = struct.unpack_from("<I", body)
        transport.on_audio(seq, body[4:])
    elif kind == FRAME_PHOTO:
        (offset,) = struct.unpack_from("<I", body)
        transport.on_photo(offset, body[4:])
    elif kind == FRAME_VIDEO:
        no, offset, total, ms = VIDEO_HEAD.unpack_from(body)
        transport.on_video(no, offset, total, ms, body[VIDEO_HEAD.size :])
    elif kind == FRAME_MIRROR:
        if mirror_sink:
            mirror_sink(body)
    elif kind == FRAME_EVENT:
        try:
            transport.on_event(json.loads(body))
        except ValueError:
            pass


def split(buf: bytes, on_text=None) -> tuple[list[tuple[int, bytes]], bytes]:
    """buf 에서 완성된 프레임들을 꺼내고, 남은 바이트를 돌려준다. 프레임 사이의 글자는 on_text 로."""
    frames = []
    while True:
        start = buf.find(MAGIC)
        if start < 0:
            # 다음에 읽을 데이터와 이어질 수 있으니 마지막 한 바이트는 남겨 둔다
            if on_text:
                buf = on_text(buf[:-1], False) + buf[-1:]
            else:
                buf = buf[-1:]
            break
        if start:
            buf = (on_text(buf[:start], True) if on_text else b"") + buf[start:]
            continue
        if len(buf) < 5:
            break
        kind, length = struct.unpack_from("<BH", buf, 2)
        if length > MAX_FRAME:
            buf = buf[2:]
            continue
        if len(buf) < 5 + length:
            break
        frames.append((kind, buf[5 : 5 + length]))
        buf = buf[5 + length :]
    return frames, buf
