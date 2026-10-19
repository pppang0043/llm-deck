import os

import pytest

import photos


@pytest.fixture
def receiver(tmp_path, monkeypatch):
    monkeypatch.setattr(photos, "FALLBACK_ROOT", tmp_path / "fallback")
    return photos.PhotoReceiver({"dir": str(tmp_path / "pics")}, tmp_path / "vids")


def send(r, name, data, step=1024):
    r.on_event({"photo": {"name": name, "size": len(data)}})
    for off in range(0, len(data), step):
        r.on_chunk(off, data[off:off + step])


def names(folder):
    return sorted(p.name for p in folder.iterdir())


def test_photo_chunked_receive(receiver, tmp_path):
    data = os.urandom(10_000)
    send(receiver, "IMG_1.jpg", data, step=777)
    assert (tmp_path / "pics/IMG_1.jpg").read_bytes() == data
    assert receiver.buf is None and receiver.pending_toast is None


def test_photo_out_of_order_chunks(receiver, tmp_path):
    data = os.urandom(3000)
    receiver.on_event({"photo": {"name": "a.jpg", "size": 3000}})
    receiver.on_chunk(2000, data[2000:])
    receiver.on_chunk(0, data[:2000])
    assert (tmp_path / "pics/a.jpg").read_bytes() == data


def test_chunk_past_end_ignored(receiver, tmp_path):
    receiver.on_event({"photo": {"name": "a.jpg", "size": 10}})
    receiver.on_chunk(5, b"x" * 10)  # 끝을 넘는다
    receiver.on_chunk(-1, b"x")
    assert not (tmp_path / "pics").exists() or names(tmp_path / "pics") == []


def test_video_goes_to_video_dir_via_temp(receiver, tmp_path):
    data = os.urandom(300_000)
    receiver.on_event({"photo": {"name": "VID_1.mov", "size": len(data)}})
    receiver.on_chunk(0, data[:1000])
    # 받는 중에는 임시 파일만 있다
    assert [p.name.startswith(".receiving-") for p in (tmp_path / "vids").iterdir()] == [True]
    for off in range(1000, len(data), 4096):
        receiver.on_chunk(off, data[off:off + 4096])
    assert names(tmp_path / "vids") == ["VID_1.mov"]
    assert (tmp_path / "vids/VID_1.mov").read_bytes() == data
    assert not (tmp_path / "pics").exists()


@pytest.mark.parametrize("name", ["IMG_1.jpg", "VID_1.mov"])
def test_duplicate_same_content_skipped(receiver, tmp_path, name):
    folder = tmp_path / ("vids" if name.endswith(".mov") else "pics")
    data = os.urandom(5000)
    send(receiver, name, data)
    send(receiver, name, data)
    assert names(folder) == [name]
    assert receiver.pending_toast and "이미" in receiver.pending_toast


@pytest.mark.parametrize("name", ["IMG_1.jpg", "VID_1.mov"])
def test_same_name_different_content_gets_suffix(receiver, tmp_path, name):
    folder = tmp_path / ("vids" if name.endswith(".mov") else "pics")
    first, second = os.urandom(5000), os.urandom(5000)
    send(receiver, name, first)
    send(receiver, name, second)
    stem, ext = os.path.splitext(name)
    assert names(folder) == sorted([name, f"{stem}_1{ext}"])
    assert (folder / f"{stem}_1{ext}").read_bytes() == second
    # _1 과 같은 내용을 다시 받으면 건너뛴다
    receiver.pending_toast = None
    send(receiver, name, second)
    assert len(names(folder)) == 2 and receiver.pending_toast


def test_interrupted_video_temp_removed_on_next(receiver, tmp_path):
    receiver.on_event({"photo": {"name": "VID_2.mov", "size": 5000}})
    receiver.on_chunk(0, b"x" * 1000)
    send(receiver, "IMG_1.jpg", b"jpg" * 100)
    assert names(tmp_path / "vids") == []
    assert (tmp_path / "pics/IMG_1.jpg").exists()


def test_size_limits(receiver):
    receiver.on_event({"photo": {"name": "a.jpg", "size": photos.PHOTO_MAX + 1}})
    assert receiver.buf is None and receiver.tmp is None
    receiver.on_event({"photo": {"name": "a.jpg", "size": 0}})
    assert receiver.buf is None
    receiver.on_event({"photo": {"name": "a.jpg", "size": "abc"}})
    assert receiver.buf is None
    receiver.on_event({"photo": {"name": "a.mov", "size": photos.VIDEO_MAX + 1}})
    assert receiver.tmp is None
    receiver.on_event({"photo": {"name": "a.mov", "size": 30_000_000}})  # 사진 한도보다 크지만 동영상이라 받는다
    assert receiver.tmp is not None
    receiver._drop()


def test_url_events_ignored(receiver):
    receiver.on_event({"photo": {"name": "a.jpg", "size": 10, "url": "http://x"}})
    receiver.on_event({"other": 1})
    receiver.on_event("not a dict")
    assert receiver.buf is None


@pytest.mark.parametrize("name, expected", [
    ("../x", "x"),
    ("../../etc/passwd", "passwd"),
    ("a/b/c.jpg", "c.jpg"),
    ("사진 1.jpg", "___1.jpg"),
    ("evil;rm -rf.jpg", "evil_rm_-rf.jpg"),
    ("", "photo.jpg"),
])
def test_unique_path_sanitizes(tmp_path, name, expected):
    path = photos.unique_path(tmp_path, name, "photo.jpg")
    assert path.parent == tmp_path
    assert path.name == expected


def test_unique_path_dotdot_stays_inside(tmp_path):
    path = photos.unique_path(tmp_path / "in", "..", "photo.jpg")
    assert path.resolve().parent == (tmp_path / "in").resolve()


def test_unique_path_numbers(tmp_path):
    (tmp_path / "a.jpg").write_bytes(b"1")
    (tmp_path / "a_1.jpg").write_bytes(b"2")
    assert photos.unique_path(tmp_path, "a.jpg", "d").name == "a_2.jpg"


def test_open_new_falls_back(tmp_path, monkeypatch):
    monkeypatch.setattr(photos, "FALLBACK_ROOT", tmp_path / "fb")
    blocked = tmp_path / "file"
    blocked.write_bytes(b"")  # 폴더 자리에 파일이 있어 만들 수 없다
    path, f = photos.open_new(blocked / "sub", "a.jpg", "d.jpg", "photos")
    f.close()
    assert path == tmp_path / "fb/photos/a.jpg"
