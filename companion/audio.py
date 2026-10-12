"""기기에서 온 마이크 소리를 내보내는 곳.

가상 오디오 장치(Mac: BlackHole, Windows: VB-CABLE)에 소리를 넣으면
다른 프로그램에서 그 장치를 "마이크"로 고를 수 있다.
형식은 펌웨어의 mic.h 와 맞춘다: 16kHz, 16bit, 모노, 한 번에 320샘플(20ms).
"""

import queue
import wave

SAMPLE_RATE = 16000
FRAME_SAMPLES = 320
DEFAULT_OUTPUTS = ("BlackHole", "CABLE Input")
# 쌓아 둘 수 있는 최대 묶음 수. 넘치면 오래된 소리를 버려 지연이 늘어나지 않게 한다.
MAX_QUEUED = 8


class Player:
    """받은 소리를 가상 오디오 장치로 내보낸다."""

    def __init__(self, cfg: dict):
        import numpy as np
        import sounddevice as sd

        wanted = cfg.get("output", "")
        names = (wanted,) if wanted else DEFAULT_OUTPUTS
        self.device_name = None
        for index, dev in enumerate(sd.query_devices()):
            if dev["max_output_channels"] > 0 and any(n.lower() in dev["name"].lower() for n in names):
                self.device_name = dev["name"]
                break
        if self.device_name is None:
            print("[audio] 가상 오디오 장치가 없어 마이크 소리는 버립니다. (Mac: BlackHole, Windows: VB-CABLE 설치 필요)")
            return

        self.frames: queue.Queue[bytes] = queue.Queue()

        def callback(outdata, n, time_info, status):
            try:
                outdata[:, 0] = np.frombuffer(self.frames.get_nowait(), dtype="<i2")
            except queue.Empty:
                outdata.fill(0)

        self.stream = sd.OutputStream(
            samplerate=SAMPLE_RATE, channels=1, dtype="int16", blocksize=FRAME_SAMPLES, device=index, callback=callback
        )
        self.stream.start()
        print(f"[audio] 마이크 소리를 '{self.device_name}' 로 내보냅니다.")

    def push(self, seq: int, pcm: bytes) -> None:
        if self.device_name is None or len(pcm) != FRAME_SAMPLES * 2:
            return
        while self.frames.qsize() >= MAX_QUEUED:
            try:
                self.frames.get_nowait()
            except queue.Empty:
                break
        self.frames.put(pcm)


class Recorder:
    """가상 오디오 장치 없이 수신만 확인하고 싶을 때: 받은 소리를 WAV 파일로 저장한다."""

    def __init__(self, path: str):
        self.path = path
        self.wav = wave.open(path, "wb")
        self.wav.setnchannels(1)
        self.wav.setsampwidth(2)
        self.wav.setframerate(SAMPLE_RATE)
        self.count = 0
        self.lost = 0
        self.last_seq = None

    def push(self, seq: int, pcm: bytes) -> None:
        if self.last_seq is not None and 1 < seq - self.last_seq < 1000:
            self.lost += seq - self.last_seq - 1
        self.last_seq = seq
        self.count += 1
        self.wav.writeframes(pcm)

    def close(self) -> None:
        self.wav.close()
        seconds = self.count * FRAME_SAMPLES / SAMPLE_RATE
        print(f"[audio] {self.count}개 수신 ({seconds:.1f}초 분량), 유실 {self.lost}개 -> {self.path}")
