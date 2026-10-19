import struct

from transports import frames


class FakeTransport:
    name = "test"

    def __init__(self):
        self.got = []

    def on_audio(self, seq, pcm):
        self.got.append(("audio", seq, pcm))

    def on_event(self, event):
        if event.get("boom"):
            raise ZeroDivisionError("handler failed")
        self.got.append(("event", event))

    def on_photo(self, offset, data):
        self.got.append(("photo", offset, data))

    def on_video(self, no, offset, total, ms, data):
        self.got.append(("video", no, offset, total, ms, data))


def frame(kind, body):
    return frames.MAGIC + struct.pack("<BH", kind, len(body)) + body


def test_dispatch_routes_each_kind():
    t = FakeTransport()
    frames.dispatch(t, frames.FRAME_AUDIO, struct.pack("<I", 7) + b"pcm")
    frames.dispatch(t, frames.FRAME_PHOTO, struct.pack("<I", 100) + b"jpg")
    frames.dispatch(t, frames.FRAME_VIDEO, frames.VIDEO_HEAD.pack(1, 2, 3, 4) + b"v")
    frames.dispatch(t, frames.FRAME_EVENT, b'{"a": 1}')
    assert t.got == [("audio", 7, b"pcm"), ("photo", 100, b"jpg"), ("video", 1, 2, 3, 4, b"v"),
                     ("event", {"a": 1})]


def test_dispatch_survives_malformed_and_handler_errors(capsys, monkeypatch):
    monkeypatch.setattr(frames, "_reported", {})
    t = FakeTransport()
    for _ in range(3):
        frames.dispatch(t, frames.FRAME_AUDIO, b"\x01")      # 너무 짧다 (struct.error)
        frames.dispatch(t, frames.FRAME_VIDEO, b"\x01\x02")
        frames.dispatch(t, frames.FRAME_EVENT, b"not json")  # 무시
        frames.dispatch(t, frames.FRAME_EVENT, b'{"boom": 1}')  # 받는 쪽 오류
        frames.dispatch(t, 99, b"unknown kind")
    frames.dispatch(t, frames.FRAME_EVENT, b'{"ok": 1}')
    assert t.got == [("event", {"ok": 1})]
    # 같은 오류의 자세한 내용은 한 번만 남긴다
    captured = capsys.readouterr()
    assert captured.err.count("ZeroDivisionError: handler failed") == 1
    assert captured.out.count("처리하지 못했습니다") == 3  # 오류 세 가지, 각각 한 번씩


def test_split_frames_text_and_partial():
    texts = []

    def on_text(data, complete):
        texts.append(data)
        return b""

    buf = b"log line\n" + frame(2, b'{"x":1}') + frame(1, b"abcd") + frame(3, b"12345")[:6]
    out, rest = frames.split(buf, on_text)
    assert out == [(2, b'{"x":1}'), (1, b"abcd")]
    assert rest == frame(3, b"12345")[:6]
    assert texts == [b"log line\n"]
    out, rest = frames.split(rest + frame(3, b"12345")[6:])
    assert out == [(3, b"12345")] and rest == b""


def test_split_skips_oversized_length():
    bad = frames.MAGIC + struct.pack("<BH", 2, frames.MAX_FRAME + 1)
    out, rest = frames.split(bad + frame(2, b"{}"))
    assert out == [(2, b"{}")]
