"""PC 마이크 녹음만 맡는 작은 프로세스. assistant.py 가 띄우고 표준 입출력으로 JSON 한 줄씩 주고받는다.

PortAudio 는 처음 켤 때 본 장치만 기억하고, 마이크를 연 뒤에 다시 켜면 (sd._terminate) 그 프로세스가 끝날 때까지
어느 마이크에서도 소리가 오지 않는다 (Mac 에서 확인). 그래서 마이크는 이 프로세스에서만 열고,
장치 목록을 새로 읽어야 하면 이 프로세스만 다시 띄운다 (도우미 전체를 다시 띄우면 기기 연결·미러링이 끊긴다).

    도우미 -> 이 프로세스: {"op": "devices"} / {"op": "listen", "name": 마이크} / {"op": "halt"} / {"op": "close"}
    이 프로세스 -> 도우미: {"devices": [이름...], "default": 기본 마이크 이름}
                          {"opened": 이름, "secs": 여는 데 걸린 초} 또는 {"error": 까닭}, 그 뒤 {"pcm": base64 int16}
                          {"halted": true}, {"closed": true}
"""

import base64
import json
import sys
import threading
import time
from pathlib import Path

# Windows 설치(windows/install.py)는 가상 환경 없이 이 PC 의 Python 으로 돌고, 부품은 이 폴더의 lib 에 풀어 둔다
_LIB = Path(__file__).resolve().parent / "lib"
if _LIB.is_dir() and str(_LIB) not in sys.path:
    sys.path.insert(1, str(_LIB))

SAMPLE_RATE = 16000
CHUNK = SAMPLE_RATE // 10


class MicServer:
    """마이크 하나를 열어 두고 듣는 동안 소리 조각을 emit 으로 보낸다. 여는 데 10초 넘게 걸릴 때가 있어서
    한 번 연 마이크는 멈춰 두었다가 다시 쓴다 (멈춘 동안은 마이크를 쓰지 않는다)."""

    def __init__(self, sd, emit):
        self.sd, self.emit = sd, emit
        self.stream = None
        self.name = ""
        self.listening = threading.Event()
        self.reader: threading.Thread | None = None

    def handle(self, cmd: dict) -> None:
        op = cmd.get("op")
        if op == "devices":
            self.emit({"devices": self._inputs(), "default": self._default()})
        elif op == "listen":
            self._listen(str(cmd.get("name", "")))
        elif op == "halt":
            self._halt()
            self.emit({"halted": True})
        elif op == "close":
            self._halt()
            self._close()
            self.emit({"closed": True})

    def _usable(self, d: dict) -> bool:
        # PortAudio WDM-KS 는 blocking 읽기를 지원하지 않는다.
        return d["max_input_channels"] > 0 and "WDM-KS" not in self.sd.query_hostapis(d["hostapi"])["name"]

    def _inputs(self) -> list[str]:
        """기본 방식(Windows 는 MME, WASAPI ... 중 하나)의 마이크 이름들"""
        devices = [d for d in self.sd.query_devices() if self._usable(d)]
        try:
            hostapi = self.sd.query_devices(kind="input")["hostapi"]
            devices = [d for d in devices if d["hostapi"] == hostapi] or devices
        except Exception:
            pass
        return [str(d["name"]) for d in devices]

    def _default(self) -> str:
        try:
            d = self.sd.query_devices(kind="input")
            return str(d["name"]) if self._usable(d) else ""
        except Exception:  # 기본 마이크가 없다
            return ""

    def _index(self, name: str) -> int | None:
        """이름이 같은 마이크의 장치 번호. Windows 는 같은 마이크가 여러 방식으로 보여서 기본 방식 것을 고른다."""
        try:
            hostapi = self.sd.query_devices(kind="input")["hostapi"]
        except Exception:
            hostapi = None
        found = [i for i, d in enumerate(self.sd.query_devices()) if self._usable(d) and d["name"] == name]
        same = [i for i in found if self.sd.query_devices(i)["hostapi"] == hostapi]
        return (same or found or [None])[0]

    def _listen(self, name: str) -> None:
        self._halt()
        try:
            started = time.monotonic()
            if self.stream is None or self.name != name:
                self._close()
                index = self._index(name)
                if index is None:  # None 을 넘기면 PortAudio 가 다른 마이크(처음 본 기본 마이크)를 연다
                    raise OSError(f"'{name}' 마이크가 장치 목록에 없습니다")
                self.stream = self.sd.InputStream(samplerate=SAMPLE_RATE, channels=1, dtype="int16", device=index)
                self.name = name
            self.stream.start()
        except Exception as e:  # 장치는 보이는데 열리지 않을 때 (다른 프로그램이 독점, 드라이버 문제 등)
            self._close()
            self.emit({"error": str(e)[:200]})
            return
        self.emit({"opened": name, "secs": round(time.monotonic() - started, 1)})
        self.listening.set()
        self.reader = threading.Thread(target=self._read, args=(self.stream,), daemon=True)
        self.reader.start()

    def _read(self, stream) -> None:
        # 기다리지 않고 읽는다: 마이크 권한을 묻는 중이거나 막혀 있으면 소리가 아예 오지 않아 read() 가 멈춰 버린다
        while self.listening.is_set():
            try:
                if stream.read_available < CHUNK:
                    time.sleep(0.02)
                    continue
                data, _ = stream.read(CHUNK)
            except Exception as e:
                self.emit({"error": str(e)[:200]})
                return
            self.emit({"pcm": base64.b64encode(data.tobytes()).decode()})

    def _halt(self) -> None:
        """녹음을 멈춘다. Mac 에서 stop() 이 돌아오지 않은 적이 있어 abort() 를 쓴다 (쌓인 소리도 버린다).
        멈추지 않으면 도우미가 기다리다 이 프로세스를 끝낸다."""
        if not self.listening.is_set():
            return
        self.listening.clear()
        if self.reader:
            self.reader.join()
        try:
            self.stream.abort()
        except Exception:
            pass

    def _close(self) -> None:
        stream, self.stream, self.name = self.stream, None, ""
        if stream is not None:
            try:
                stream.close()
            except Exception:
                pass


def main() -> None:
    import sounddevice as sd

    lock = threading.Lock()

    def emit(message: dict) -> None:
        with lock:
            sys.stdout.write(json.dumps(message, ensure_ascii=False) + "\n")
            sys.stdout.flush()

    server = MicServer(sd, emit)
    for line in sys.stdin:  # 도우미가 끝나면 입력이 닫혀 이 프로세스도 끝난다
        try:
            server.handle(json.loads(line))
        except ValueError:
            continue
    server._halt()
    server._close()


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stdin.reconfigure(encoding="utf-8")
    main()
