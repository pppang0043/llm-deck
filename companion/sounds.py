"""기기가 보낸 알림({"sound": 이름})에 맞춰 PC 스피커로 짧은 소리를 낸다.

소리 파일은 설정의 [sounds] 에서 이름별로 바꿀 수 있다. 적지 않으면 기본 효과음을 만들어 쓴다.
    [sounds]
    enabled = true
    mic_on = "C:/Windows/Media/Speech On.wav"
"""

import math
import shutil
import struct
import subprocess
import sys
import threading
import wave
from pathlib import Path

SOUND_DIR = Path.home() / ".llm-deck" / "sounds"
RATE = 22050

# 기본 효과음: (주파수 Hz, 길이 초) 를 차례로 이어 붙인다
TONES = {
    "mic_on": [(880, 0.07), (1320, 0.11)],  # 올라가는 "띠링"
    "mic_off": [(1320, 0.07), (880, 0.11)],  # 내려가는 "띠롱"
    "limit_warn": [(988, 0.09), (0, 0.06), (988, 0.09), (0, 0.06), (988, 0.12)],  # 짧게 세 번 "삐삐삐"
    "limit_reset": [(784, 0.08), (988, 0.08), (1175, 0.08), (1568, 0.16)],  # 올라가는 "도미솔도"
}


def _make_wav(path: Path, tones: list[tuple[int, float]]) -> None:
    frames = bytearray()
    for freq, seconds in tones:
        n = int(RATE * seconds)
        fade = int(RATE * 0.01)
        for i in range(n):
            # 끝을 부드럽게 줄여 "딱" 하는 잡음이 나지 않게 한다
            gain = min(1.0, i / fade, (n - i) / fade)
            # freq 0 은 쉼표
            value = 12000 * gain * math.sin(2 * math.pi * freq * i / RATE) if freq else 0
            frames += struct.pack("<h", int(value))
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))


def _file_for(name: str, cfg: dict) -> Path | None:
    if cfg.get(name):
        return Path(cfg[name]).expanduser()
    if name not in TONES:
        return None
    path = SOUND_DIR / f"{name}.wav"
    if not path.exists():
        _make_wav(path, TONES[name])
    return path


def _play_file(path: Path) -> None:
    if sys.platform == "win32":
        import winsound

        winsound.PlaySound(str(path), winsound.SND_FILENAME | winsound.SND_ASYNC)
    elif sys.platform == "darwin":
        subprocess.Popen(["afplay", str(path)])
    elif shutil.which("aplay"):
        subprocess.Popen(["aplay", "-q", str(path)])


class Sounds:
    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.enabled = cfg.get("enabled", True)

    def play(self, name: str) -> None:
        if not self.enabled:
            return
        try:
            path = _file_for(name, self.cfg)
            if path is None or not path.exists():
                print(f"[sound] '{name}' 소리 파일이 없습니다: {path}")
                return
            _play_file(path)
        except Exception as e:  # 소리가 안 나도 사용량 전송은 계속되어야 한다
            print(f"[sound] 재생 실패: {e}")

    def on_event(self, event: dict) -> None:
        """transport 가 기기 알림을 받으면 부른다 (수신 스레드에서 불리므로 바로 돌려준다)."""
        if isinstance(event, dict) and isinstance(event.get("sound"), str):
            print(f"[sound] 기기 알림: {event['sound']}")
            threading.Thread(target=self.play, args=(event["sound"],), daemon=True).start()
