"""기기가 Wi-Fi 로 보내는 마이크 소리를 받아 가상 오디오 장치로 내보낸다.

가상 오디오 장치(Mac: BlackHole, Windows: VB-CABLE)에 소리를 넣으면
다른 프로그램에서 그 장치를 "마이크"로 고를 수 있다.
형식은 펌웨어의 mic.h 와 맞춘다: 16kHz, 16bit, 모노, 패킷당 320샘플.
"""

import queue
import socket
import struct
import threading
import time
import wave

SAMPLE_RATE = 16000
FRAME_SAMPLES = 320
UDP_PORT = 4010
HEADER = struct.Struct("<I")
DEFAULT_OUTPUTS = ("BlackHole", "CABLE Input")
# 쌓아 둘 수 있는 최대 패킷 수. 넘치면 오래된 소리를 버려 지연이 늘어나지 않게 한다.
MAX_QUEUED = 8


def _open_socket() -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", UDP_PORT))
    return sock


def _find_output(sd, wanted: str):
    names = (wanted,) if wanted else DEFAULT_OUTPUTS
    for index, dev in enumerate(sd.query_devices()):
        if dev["max_output_channels"] > 0 and any(n.lower() in dev["name"].lower() for n in names):
            return index, dev["name"]
    return None, None


def start(cfg: dict) -> None:
    """백그라운드에서 수신을 시작한다. 가상 오디오 장치가 없으면 안내만 하고 넘어간다."""
    import numpy as np
    import sounddevice as sd

    device, name = _find_output(sd, cfg.get("output", ""))
    if device is None:
        print("[audio] 가상 오디오 장치를 찾지 못해 마이크 기능을 끕니다. (Mac: BlackHole, Windows: VB-CABLE 설치 필요)")
        return

    frames: queue.Queue[bytes] = queue.Queue()

    def callback(outdata, n, time_info, status):
        try:
            outdata[:, 0] = np.frombuffer(frames.get_nowait(), dtype="<i2")
        except queue.Empty:
            outdata.fill(0)

    def receive():
        sock = _open_socket()
        while True:
            packet = sock.recv(2048)
            pcm = packet[HEADER.size :]
            if len(pcm) != FRAME_SAMPLES * 2:
                continue
            while frames.qsize() >= MAX_QUEUED:
                frames.get_nowait()
            frames.put(pcm)

    stream = sd.OutputStream(
        samplerate=SAMPLE_RATE, channels=1, dtype="int16", blocksize=FRAME_SAMPLES, device=device, callback=callback
    )
    stream.start()
    threading.Thread(target=receive, daemon=True).start()
    print(f"[audio] 마이크 소리를 '{name}' 로 내보냅니다.")


def record(path: str, seconds: float) -> None:
    """가상 오디오 장치 없이 수신만 확인하고 싶을 때: 받은 소리를 WAV 파일로 저장한다."""
    sock = _open_socket()
    sock.settimeout(0.5)
    packets = lost = 0
    last_seq = None
    end = time.time() + seconds
    with wave.open(path, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        while time.time() < end:
            try:
                packet = sock.recv(2048)
            except socket.timeout:
                continue
            (seq,) = HEADER.unpack_from(packet)
            if last_seq is not None and seq > last_seq + 1:
                lost += seq - last_seq - 1
            last_seq = seq
            packets += 1
            wav.writeframes(packet[HEADER.size :])
    print(f"[audio] {packets} 패킷 수신 ({packets * FRAME_SAMPLES / SAMPLE_RATE:.1f}초 분량), 유실 {lost} -> {path}")
