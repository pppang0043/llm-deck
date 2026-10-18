"""기기가 USB 와 Wi-Fi(TCP)로 보내는 묶음(프레임) 형식. 펌웨어의 usb_link.h 와 맞춘다.

    [0xA5 0x5A][종류 1바이트][길이 2바이트][내용]
"""

import json
import struct

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


def dispatch(transport, kind: int, body: bytes) -> None:
    """프레임 하나를 transport 의 on_audio / on_event / on_photo / on_video 로 넘긴다."""
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
