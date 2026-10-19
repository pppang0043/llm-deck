import struct
import time
import types

import pytest

import photos
import videos


def jpeg(w=640, h=480, fill=0, pad=50):
    sof = b"\xff\xc0\x00\x11\x08" + struct.pack(">HH", h, w) + b"\x03" + bytes(9)
    return b"\xff\xd8" + sof + bytes([fill]) * pad + b"\xff\xd9"


def boxes(data, start=0, end=None):
    """(종류, 내용 시작, 상자 끝) 목록. 64비트 크기(size==1)도 읽는다."""
    end = len(data) if end is None else end
    out, i = [], start
    while i + 8 <= end:
        size, kind = struct.unpack(">I4s", data[i:i + 8])
        head = 8
        if size == 1:
            size = struct.unpack(">Q", data[i + 8:i + 16])[0]
            head = 16
        assert size >= head and i + size <= end, (kind, size)
        out.append((kind, i + head, i + size))
        i += size
    assert i == end, "상자 끝이 파일 끝과 맞지 않는다"
    return out


def find(data, path):
    start, end = 0, len(data)
    for kind in path:
        match = [b for b in boxes(data, start, end) if b[0] == kind]
        assert match, f"{kind} 없음"
        _, start, end = match[0]
    return start, end


def check_mov(path, frames):
    data = path.read_bytes()
    top = boxes(data)
    assert [b[0] for b in top] == [b"ftyp", b"mdat", b"moov"]
    _, mdat_body, mdat_end = top[1]
    assert mdat_body - 16 == top[0][2]  # 64비트 크기 칸을 쓰고, 크기가 moov 직전까지로 맞다 (boxes 가 확인)
    assert sum(len(f) for f in frames) == mdat_end - mdat_body
    s, _ = find(data, [b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stsz"])
    _, sample_size, count = struct.unpack(">III", data[s:s + 12])
    assert count == len(frames)
    sizes = struct.unpack(f">{count}I", data[s + 12:s + 12 + 4 * count])
    assert list(sizes) == [len(f) for f in frames]
    c, _ = find(data, [b"moov", b"trak", b"mdia", b"minf", b"stbl", b"co64"])
    _, n = struct.unpack(">II", data[c:c + 8])
    assert n == len(frames)
    offsets = struct.unpack(f">{n}Q", data[c + 8:c + 8 + 8 * n])
    for off, size, frame in zip(offsets, sizes, frames):
        assert data[off:off + size] == frame
    return data


def test_movwriter_valid(tmp_path):
    frames = [jpeg(fill=i, pad=50 + i * 10) for i in range(5)]
    w = videos.MovWriter(tmp_path / "a.mov", turns=1)
    for i, f in enumerate(frames):
        w.add(f, i * 100)
    assert w.close() == 5
    data = check_mov(tmp_path / "a.mov", frames)
    # tkhd 끝의 가로·세로 (16.16)
    _, end = find(data, [b"moov", b"trak", b"tkhd"])
    assert struct.unpack(">II", data[end - 8:end]) == (640 << 16, 480 << 16)
    assert w.width == 640 and w.height == 480


def test_movwriter_empty_has_no_moov(tmp_path):
    w = videos.MovWriter(tmp_path / "e.mov", turns=0)
    assert w.close() == 0
    assert [b[0] for b in boxes((tmp_path / "e.mov").read_bytes())] == [b"ftyp", b"mdat"]


def test_jpeg_size_fallback():
    assert videos._jpeg_size(jpeg(320, 240)) == (320, 240)
    assert videos._jpeg_size(b"\xff\xd8garbage") == (640, 480)


@pytest.fixture
def receiver(tmp_path, monkeypatch):
    monkeypatch.setattr(photos, "FALLBACK_ROOT", tmp_path / "fallback")
    return videos.VideoReceiver({"dir": str(tmp_path / "vids")})


def send_frame(r, no, frame, ms, step=40):
    for off in range(0, len(frame), step):
        r.on_chunk(no, off, len(frame), ms, frame[off:off + step])


def test_receiver_start_end(receiver, tmp_path):
    frames = [jpeg(fill=i) for i in range(3)]
    receiver.on_event({"video": {"start": "VID_a.mov", "turns": 1}})
    for i, f in enumerate(frames):
        send_frame(receiver, i, f, i * 66)
    send_frame(receiver, 2, frames[2], 132)  # 같은 장을 다시 보내도 한 번만 들어간다
    receiver.on_event({"video": {"end": "VID_a.mov", "frames": 3, "dropped": 0}})
    assert receiver.writer is None
    check_mov(tmp_path / "vids/VID_a.mov", frames)


def test_finish_on_interrupted_recording(receiver, tmp_path):
    frames = [jpeg(fill=i) for i in range(2)]
    receiver.on_event({"video": {"start": "VID_b.mov"}})
    for i, f in enumerate(frames):
        send_frame(receiver, i, f, i * 100)
    partial = jpeg(fill=9)
    receiver.on_chunk(2, 0, len(partial), 200, partial[:20])  # 끊긴 장
    receiver.finish()
    assert receiver.writer is None
    check_mov(tmp_path / "vids/VID_b.mov", frames)


def test_finish_without_frames_removes_file(receiver, tmp_path):
    receiver.on_event({"video": {"start": "VID_c.mov"}})
    receiver.finish()
    assert list((tmp_path / "vids").iterdir()) == []


def test_new_start_finishes_previous(receiver, tmp_path):
    receiver.on_event({"video": {"start": "VID_d.mov"}})
    send_frame(receiver, 0, jpeg(), 0)
    receiver.on_event({"video": {"start": "VID_e.mov"}})
    check_mov(tmp_path / "vids/VID_d.mov", [jpeg()])
    receiver.finish()


def test_oversized_frame_ignored(receiver):
    receiver.on_event({"video": {"start": "VID_f.mov"}})
    receiver.on_chunk(0, 0, 5_000_000, 0, b"x" * 10)
    assert receiver.writer.sizes == []
    receiver.finish()


def test_idle_timeout_finishes(receiver, tmp_path, monkeypatch):
    real_sleep = time.sleep
    monkeypatch.setattr(videos, "IDLE_SECONDS", 0.05)
    monkeypatch.setattr(videos, "time", types.SimpleNamespace(sleep=lambda s: real_sleep(0.02),
                                                              monotonic=time.monotonic))
    receiver.on_event({"video": {"start": "VID_g.mov"}})
    send_frame(receiver, 0, jpeg(), 0)
    deadline = time.monotonic() + 3
    while receiver.writer is not None and time.monotonic() < deadline:
        real_sleep(0.02)
    assert receiver.writer is None
    check_mov(tmp_path / "vids/VID_g.mov", [jpeg()])
