"""기기 마이크를 질문 녹음에 먼저 쓴다 (assistant._record_device). 기기는 녹음 동안 20ms 묶음을 보내고 "dev_mic" 로 알린다.
마이크가 없으면 기기 I2S 는 늘 같은 값(-1)만 읽으므로, 그때는 PC 마이크로 녹음한다."""

import threading
import time

import numpy as np

import assistant
from test_assistant_mic import fake, mic_files  # noqa: F401 (fixture)


def feed(a, take, pcm_of, seconds):
    """기기처럼 20ms 마다 소리를 보낸다"""
    def run():
        i = 0
        end = time.monotonic() + seconds
        while time.monotonic() < end and take.recording.is_set():
            a.on_device_audio(i, pcm_of(i))
            i += 1
            time.sleep(0.02)
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def record(a, pcm_of, seconds):
    take = assistant._Take()
    take.dev_mic = True
    a.take = take
    feed(a, take, pcm_of, seconds + 1)
    t = threading.Thread(target=a._record, args=(take,))
    t.start()
    time.sleep(seconds)
    a._end_take(take)
    t.join(5)
    return take


def noisy(i):
    return np.random.default_rng(i).integers(-200, 200, 320, dtype=np.int16).astype("<i2").tobytes()


def flat(i):
    return np.full(320, -1, dtype="<i2").tobytes()


def test_device_mic_used_first(fake):
    sd, a = fake
    take = record(a, noisy, 1.0)
    assert take.mic == assistant.DEVICE_MIC
    assert len(take.chunks) > 20 and take.chunks[0].shape == (320, 1)
    assert a.pending_toast is None


def test_flat_device_mic_falls_back_to_pc(fake):
    sd, a = fake
    take = record(a, flat, 4.0)
    assert take.mic == "MacBook Pro 마이크"
    assert take.chunks and int(np.concatenate(take.chunks).max()) == 100  # PC 마이크 소리만 남는다


def test_audio_ignored_when_not_recording_from_device(fake):
    sd, a = fake
    a.on_device_audio(0, noisy(0))
    assert a._dev_audio.empty()
