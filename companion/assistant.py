"""기기에서 말로 묻고 글로 답받기 (Claude Code CLI 를 구독 계정으로 쓴다).

흐름: 기기 {"ask": "start"} -> 이 PC 마이크로 녹음 -> {"ask": "stop"} -> 글자로 바꾸기 (아래 "받아 적기")
      -> 미리 띄워 둔 CLI 에 질문 -> 답을 나오는 대로 기기로 보낸다 {"ask": {"state", "q", "a", "done"}}
      {"ask": "cancel"} 녹음 버리기, {"ask": "new"} 새 대화 (앞 대화를 잊는다)
      {"ask": "stop", "voice": true} 면 답이 다 나온 뒤 이 PC 스피커로 읽어 준다 (speech.py). {"ask": "mute"} 읽기 멈춤
      읽는 것은 답의 앞 1~2문장(요약)이다. 질문에 "길게", "자세히" 같은 말이 있으면 답 전체를 읽는다.
받아쓰기: {"ask": "start", "mode": "dictate"} ... {"ask": "stop"} -> 받아 적은 글을 이 PC 의 커서 자리에 입력한다
      (Claude 에 묻지 않는다, Enter 는 누르지 않는다). 기기 사용량 화면의 버튼이 쓴다.
      "orca": 세션 id 가 있으면 그 ORCA 세션을 앞으로 띄우고 그 세션 터미널에 넣는다 (orca.py, 기기의 ORCA 화면).
      "슬래시 커밋" 처럼 말하면 "/commit" 으로 바꿔 넣는다.

받아 적기: CLI 는 소리를 받지 못하므로 음성은 먼저 글자로 바꾼다. 말소리가 없는 녹음은 어느 쪽으로도 넘기지 않는다 (VAD).
      기본은 이 PC 안의 Whisper (Apple Silicon Mac 은 GPU 의 mlx-whisper, 그 밖은 CPU 의 faster-whisper) 라서
      소리가 인터넷으로 나가지 않는다. stt = "chatgpt" 면 ChatGPT 로 보내고 (chatgpt_stt.py), 실패할 때만 Whisper 를 쓴다.
      Whisper 에는 자주 쓰는 말(prompt)과 ORCA 세션의 / 명령 이름을 알려 준다. ChatGPT 로는 보낼 자리가 없다.
녹음과 처리는 따로 돈다: 앞 녹음을 받아 적거나 답을 기다리는 동안에도 새로 녹음할 수 있고,
      다 녹음한 것은 줄을 서서 (_Take) 하나씩 차례대로 받아 적고 묻는다. 기기에는 녹음 중이면 녹음 상태를,
      아니면 처리 상태를 보낸다 ("queued": 줄 서서 기다리는 녹음 수).
CLI 는 질문마다 새로 켜면 2~3초가 더 걸려서, 한 번 띄워 두고(stream-json) 계속 묻는다. 같은 프로세스라 대화도 이어진다.
30분 동안 질문이 없으면 CLI 와 음성 인식 모델을 내려 메모리를 돌려준다 (다음 질문 때 다시 띄운다).
회사 PC 는 명령 이름이 다를 수 있어서 claude-test 를 먼저 찾고, 없으면 claude 를 쓴다.
"""

import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

import chatgpt_stt
import orca
import speech
import typer

# 먼저 찾는 순서. config.toml 의 [assistant] command 로 정하면 그것만 쓴다.
COMMANDS = ("claude-test", "claude")
# 질문 전용 폴더. 대화 기록이 여기에 묶여 다른 Claude Code 작업과 섞이지 않는다 (없으면 만든다).
# 이 폴더에 CLAUDE.md 를 두면 매번 알고 있어야 할 배경을 적어 둘 수 있다.
WORKDIR = Path.home() / ".llm-deck" / "assistant"
SYSTEM_PROMPT = (
    "너는 작은 휴대 기기 화면(가로 약 20자)에 답을 보여 주는 비서다. 질문은 음성을 받아 적은 것이라 오타가 있을 수 있다. "
    "발음이 비슷한 다른 낱말로 잘못 적혔을 수 있으니(예: 수도 -> 소도), 글자 그대로보다 가장 자연스러운 뜻으로 알아듣고 답하라. "
    "답의 첫 1~2문장은 그것만 들어도 답이 되는 요약으로 써라 (소리로 읽어 줄 때 이 부분만 읽는다). "
    "한국어로, 핵심만 짧게 답하라. 표·코드 블록·마크다운 기호는 쓰지 말고 짧은 문장이나 줄바꿈으로 나눠라."
)
# 질문에 이런 말이 있으면 읽어 줄 때 요약이 아니라 답 전체를 읽는다
LONG_WORDS = ("길게", "자세히", "자세하게", "전부", "전체", "다 읽어", "풀어서", "길게 말")
SAMPLE_RATE = 16000
MAX_RECORD_SECONDS = 60
# 처리를 기다리며 줄 설 수 있는 녹음 수. 넘으면 새로 누른 것은 받지 않는다
MAX_QUEUED_TAKES = 5
# 기기 화면에 보낼 답의 최대 글자 수 (넘으면 뒤를 자른다)
MAX_ANSWER_CHARS = 1500
# 답을 기다리는 한계시간. 넘으면 CLI 를 끄고 기기에 실패로 알린다 (다음 질문은 새로 띄운 CLI 로)
ANSWER_SECONDS = 180
# 답이 나오는 동안 기기로 보내는 간격 (BLE 로도 따라갈 수 있게)
PARTIAL_SECONDS = 0.5


# 로그인 때 자동으로 켜진 도우미는 PATH 가 짧아서(터미널과 다르다) CLI 가 깔리는 자리를 직접 더 찾는다
EXTRA_DIRS = [
    "~/.local/bin", "~/.claude/local", "~/.npm-global/bin", "/opt/homebrew/bin", "/usr/local/bin",
    "%APPDATA%\\npm", "%USERPROFILE%\\.local\\bin",
]


# 받아 적을 때 앞 문맥으로 주는 낱말들. 이런 말을 더 잘 알아듣는다 (config 의 prompt 로 바꿀 수 있다)
DEFAULT_PROMPT = "받아쓰기, 펌웨어, 빌드, 커밋, 깃, 세션, 오르카, 클로드, 기기, 배터리, 코드, 스킬."
# 음성 인식이 조용한 소리·잡음에서 지어내는 말 (동영상 자막으로 배운 끝인사). 받아 적은 글 전체가 이것뿐이면 알아듣지 못한 것으로 본다
MADE_UP = {"감사합니다", "고맙습니다", "시청해주셔서감사합니다", "시청해주셔서고맙습니다", "구독과좋아요부탁드립니다",
           "다음영상에서만나요", "안녕히계세요", "thankyou", "thanksforwatching", "you"}
# 이 정도보다 작은 소리만 녹음됐으면 알아듣지 못했을 때 마이크 가까이에서 말하라고 알린다
QUIET_PEAK = 0.1


def _has_speech(pcm) -> bool:
    """녹음에 말소리가 있는지 (faster-whisper 의 VAD). 말이 없는 녹음을 넘기면 어느 엔진이든 없는 말을 지어낸다."""
    from faster_whisper.vad import get_speech_timestamps

    return bool(get_speech_timestamps(pcm))


def _heard(no_speech: float, logprob: float) -> bool:
    """한 구간이 실제 말인지 (음성 인식 스스로 "말이 아닌 것 같다"고 하면서 자신도 없으면 버린다)"""
    return not (no_speech > 0.6 and logprob < -1.0) and no_speech < 0.9


def _drop_made_up(text: str) -> str:
    """지어낸 말뿐이면 빈 글로 바꾼다"""
    bare = "".join(ch for ch in text.lower() if ch.isalnum())
    return "" if bare in MADE_UP else text


def _which(name: str) -> str | None:
    path = shutil.which(name)
    if path:
        return path
    extra = os.pathsep.join(os.path.expanduser(os.path.expandvars(d)) for d in EXTRA_DIRS)
    return shutil.which(name, path=extra)


def find_command(cfg: dict) -> tuple[str, str] | None:
    """(명령 이름, 실행 파일 경로). 실행해서 응답하는 것만 고른다. 없으면 None."""
    names = (cfg["command"],) if cfg.get("command") else COMMANDS
    for name in names:
        # Windows 는 claude-test.cmd 처럼 확장자가 붙어도 which 가 찾는다
        path = _which(name)
        if not path:
            print(f"[assistant] {name}: 없음")
            continue
        try:
            result = subprocess.run([path, "--version"], capture_output=True, text=True, timeout=30,
                                    stdin=subprocess.DEVNULL, creationflags=_NO_WINDOW)
        except (OSError, subprocess.TimeoutExpired) as e:
            print(f"[assistant] {name}: 실행 실패 ({e})")
            continue
        if result.returncode != 0:
            print(f"[assistant] {name}: 실행 실패 ({(result.stderr or result.stdout).strip()[:80]})")
            continue
        print(f"[assistant] {name}: 사용 ({result.stdout.strip()}, {path})")
        return name, path
    return None


def workdir() -> Path:
    WORKDIR.mkdir(parents=True, exist_ok=True)
    return WORKDIR


# 마이크를 열었는데 소리가 하나도 오지 않을 때 (권한)
_NO_AUDIO_REASON = (
    "PC 마이크 소리가 오지 않습니다\n시스템 설정 > 개인정보 보호 > 마이크\n에서 LLM Deck 을 켜 주세요"
    if sys.platform == "darwin" else
    "PC 마이크 소리가 오지 않습니다\n설정 > 개인 정보 > 마이크를\n확인해 주세요")
_LID_CLOSED_REASON = "맥북 덮개가 닫혀\n내장 마이크가 꺼져 있습니다\n덮개를 열거나 다른 마이크를 쓰세요"
# 장치 목록을 새로 읽으려고 도우미를 다시 띄운 시각과 까닭 (다시 띄운 도우미가 물려받는다)
_RESTARTED_ENV = "LLMDECK_AUDIO_RESTART"
# 같은 까닭으로는 이만큼 지나야 또 다시 띄운다
RESTART_AGAIN_SECONDS = 3600
# 소리가 늘 0 인 가상 장치. 마이크를 돌아가며 잡을 때 건너뛴다
_VIRTUAL = ("blackhole", "cable output", "zoomaudio", "teams audio", "soundflower")
# Windows 의 "기본 마이크" 별칭 장치 (MME 사운드 매퍼, DirectSound 주 캡처 드라이버). 진짜 기본 마이크를 알면 건너뛴다
_ALIASES = ("sound mapper", "사운드 매퍼", "primary sound capture", "주 사운드 캡처")


def _system_default_input() -> str | None:
    """시스템 설정에서 지금 고른 기본 마이크 이름. 못 읽으면 None.
    PortAudio 는 처음 켤 때의 기본 마이크만 기억하고, 마이크를 연 뒤에는 다시 켤 수 없어서 (_refresh_devices) 직접 묻는다."""
    if sys.platform == "darwin":
        return _mac_default_input()
    if sys.platform == "win32":
        return _windows_default_input()
    return None


def _mac_default_input() -> str | None:
    """Mac: 시스템 설정 > 사운드 > 입력 에서 고른 장치 (CoreAudio)."""
    import ctypes
    import ctypes.util
    from ctypes import POINTER, Structure, byref, c_char_p, c_long, c_uint32, c_void_p, sizeof

    class Address(Structure):
        _fields_ = [("selector", c_uint32), ("scope", c_uint32), ("element", c_uint32)]

    def code(text: str) -> int:
        return int.from_bytes(text.encode(), "big")

    try:
        ca = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreAudio"))
        cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
        ca.AudioObjectGetPropertyData.argtypes = [c_uint32, POINTER(Address), c_uint32, c_void_p,
                                                  POINTER(c_uint32), c_void_p]
        cf.CFStringGetCString.argtypes = [c_void_p, c_char_p, c_long, c_uint32]
        cf.CFRelease.argtypes = [c_void_p]
        device, size = c_uint32(0), c_uint32(sizeof(c_uint32))
        # 시스템(1)의 기본 입력 장치 ('dIn ')
        if ca.AudioObjectGetPropertyData(1, byref(Address(code("dIn "), code("glob"), 0)), 0, None,
                                         byref(size), byref(device)) or not device.value:
            return None
        name, size = c_void_p(), c_uint32(sizeof(c_void_p))
        if ca.AudioObjectGetPropertyData(device.value, byref(Address(code("lnam"), code("glob"), 0)), 0, None,
                                         byref(size), byref(name)) or not name.value:
            return None
        buf = ctypes.create_string_buffer(512)
        ok = cf.CFStringGetCString(name, buf, 512, 0x08000100)  # UTF-8
        cf.CFRelease(name)
        return buf.value.decode() if ok else None
    except Exception:
        return None


def _windows_default_input() -> str | None:
    """Windows: 소리 설정에서 고른 기본 녹음 장치 (Core Audio 의 IMMDeviceEnumerator, COM 을 ctypes 로 부른다)."""
    import ctypes
    from ctypes import POINTER, Structure, Union, byref, c_ubyte, c_ulong, c_ulonglong, c_ushort, c_void_p, c_wchar_p

    class GUID(Structure):
        _fields_ = [("d1", c_ulong), ("d2", c_ushort), ("d3", c_ushort), ("d4", c_ubyte * 8)]

    class PROPERTYKEY(Structure):
        _fields_ = [("fmtid", GUID), ("pid", c_ulong)]

    class PROPVARIANT(Structure):
        class _Value(Union):
            _fields_ = [("pwszVal", c_wchar_p), ("pad", c_ulonglong * 2)]

        _fields_ = [("vt", c_ushort), ("r1", c_ushort), ("r2", c_ushort), ("r3", c_ushort), ("value", _Value)]

    def guid(text: str) -> GUID:
        g = GUID()
        ctypes.windll.ole32.CLSIDFromString(c_wchar_p(text), byref(g))
        return g

    def vtable(obj: c_void_p):
        return ctypes.cast(obj, POINTER(POINTER(c_void_p))).contents

    def call(obj: c_void_p, index: int, argtypes: tuple, *args) -> None:
        """COM 함수 (vtable 의 index 번째). 실패하면 OSError (HRESULT)."""
        ctypes.WINFUNCTYPE(ctypes.HRESULT, c_void_p, *argtypes)(vtable(obj)[index])(obj, *args)

    try:
        ole32 = ctypes.windll.ole32
        ole32.CoCreateInstance.restype = ctypes.HRESULT
        initialized = ole32.CoInitializeEx(None, 0) in (0, 1)  # COINIT_MULTITHREADED (이미 했으면 S_FALSE)
    except Exception:
        return None
    enum, device, store = c_void_p(), c_void_p(), c_void_p()
    try:
        ole32.CoCreateInstance(byref(guid("{BCDE0395-E52F-467C-8E3D-C4579291692E}")), None, 23,  # MMDeviceEnumerator
                               byref(guid("{A95664D2-9614-4F35-A746-DE8DB63617E6}")), byref(enum))
        # IMMDeviceEnumerator::GetDefaultAudioEndpoint(eCapture, eConsole)
        call(enum, 4, (c_ulong, c_ulong, POINTER(c_void_p)), 1, 0, byref(device))
        call(device, 4, (c_ulong, POINTER(c_void_p)), 0, byref(store))  # IMMDevice::OpenPropertyStore(STGM_READ)
        key = PROPERTYKEY(guid("{A45C254E-DF1C-4EFD-8020-67D146A850E0}"), 14)  # PKEY_Device_FriendlyName
        value = PROPVARIANT()
        call(store, 5, (POINTER(PROPERTYKEY), POINTER(PROPVARIANT)), byref(key), byref(value))  # GetValue
        try:
            return value.value.pwszVal if value.vt == 31 else None  # VT_LPWSTR
        finally:
            ole32.PropVariantClear(byref(value))
    except Exception:  # 기본 녹음 장치가 없거나 (E_NOTFOUND) COM 을 못 쓰면 PortAudio 의 기본 마이크로
        return None
    finally:
        for obj in (store, device, enum):
            if obj:
                ctypes.WINFUNCTYPE(c_ulong, c_void_p)(vtable(obj)[2])(obj)  # Release
        if initialized:
            ole32.CoUninitialize()


def _match_input(full: str, names: list[str]) -> str | None:
    """시스템이 알려 준 마이크 이름에 맞는 PortAudio 장치 이름. Windows MME 는 이름을 31자에서 자른다."""
    if full in names:
        return full
    return next((n for n in names if len(n) >= 31 and full.startswith(n)), None)


def _input_index(sd, name: str) -> int | None:
    """이름이 같은 마이크의 장치 번호. Windows 는 같은 마이크가 여러 방식(MME, WASAPI ...)으로 보여서 기본 방식 것을 고른다."""
    try:
        hostapi = sd.query_devices(kind="input")["hostapi"]
    except Exception:
        hostapi = None
    found = [i for i, d in enumerate(sd.query_devices()) if d["max_input_channels"] > 0 and d["name"] == name]
    same = [i for i in found if sd.query_devices(i)["hostapi"] == hostapi]
    return (same or found or [None])[0]


def _lid_closed() -> bool:
    """Mac 덮개가 닫혀 있는지. 닫히면 내장 마이크는 장치 목록에 그대로 있지만 소리가 0 으로만 들어온다."""
    if sys.platform != "darwin":
        return False
    try:
        out = subprocess.run(["ioreg", "-r", "-k", "AppleClamshellState", "-d", "1"],
                             capture_output=True, text=True, timeout=3).stdout
    except (OSError, subprocess.TimeoutExpired):
        return False
    return '"AppleClamshellState" = Yes' in out

# Windows 에서 CLI 를 띄울 때 검은 창이 뜨지 않게 한다
_NO_WINDOW = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0


def spoken_text(question: str, answer: str) -> str:
    """소리로 읽을 부분: 보통은 답의 앞 1~2문장, 길게 말해 달라고 했으면 전부."""
    if any(w in question for w in LONG_WORDS):
        return answer
    import re

    # 첫 단락에서 문장 두 개까지 (마침표·물음표·느낌표·줄바꿈으로 나눈다)
    first = answer.strip().split("\n\n")[0]
    sentences = [x for x in re.split(r"(?<=[.!?。])\s+|\n+", first) if x.strip()]
    return " ".join(sentences[:2])


# Apple Silicon Mac 에서 GPU 로 받아 적을 때 쓰는 모델 (Hugging Face 이름)
MLX_MODELS = {
    "tiny": "mlx-community/whisper-tiny-mlx", "base": "mlx-community/whisper-base-mlx",
    "small": "mlx-community/whisper-small-mlx", "medium": "mlx-community/whisper-medium-mlx",
    "large-v3-turbo": "mlx-community/whisper-large-v3-turbo", "turbo": "mlx-community/whisper-large-v3-turbo",
}


def _mlx_ready() -> bool:
    if sys.platform != "darwin":
        return False
    try:
        import mlx_whisper  # noqa: F401
    except ImportError:
        return False
    return True


class _MlxModel:
    """mlx-whisper (Apple GPU). 5초 말을 large-v3-turbo 로 1초 안쪽에 받아 적는다 (CPU 는 4초쯤)."""

    def __init__(self, repo: str, cache_dir: str | None, language: str | None):
        import numpy as np
        from huggingface_hub import snapshot_download

        try:
            # 받아 둔 모델이 있으면 인터넷에 묻지 않는다
            self.path = snapshot_download(repo, cache_dir=cache_dir, local_files_only=True)
        except Exception:
            self.path = snapshot_download(repo, cache_dir=cache_dir)
        self.text(np.zeros(SAMPLE_RATE, dtype=np.float32), language, None)  # 무게를 GPU 에 올려 둔다

    def text(self, pcm, language: str | None, prompt: str | None) -> str:
        """말소리가 있는지는 부르는 쪽이 먼저 본다 (_has_speech)."""
        import mlx_whisper

        result = mlx_whisper.transcribe(pcm, path_or_hf_repo=self.path, language=language, initial_prompt=prompt,
                                        condition_on_previous_text=False)
        return "".join(s["text"] for s in result["segments"] if _heard(s["no_speech_prob"], s["avg_logprob"])).strip()


class _Held:
    """이미 잡은 잠금을 with 블록이 끝날 때 놓는다."""

    def __init__(self, lock: threading.Lock):
        self.lock = lock

    def __enter__(self) -> None:
        return None

    def __exit__(self, *exc) -> None:
        self.lock.release()


class AnswerTimeout(Exception):
    """답이 한계시간 안에 오지 않았다. (OSError 가 아니라서 _answer 가 다시 묻지 않는다)"""


class Session:
    """백그라운드에 띄워 둔 CLI 하나. 질문을 넣으면 답이 나오는 대로 on_text 를 부른다."""

    def __init__(self, path: str, cfg: dict):
        args = [path, "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
                "--include-partial-messages", "--tools", "", "--system-prompt", cfg.get("system_prompt", SYSTEM_PROMPT)]
        if cfg.get("model", "sonnet"):
            args += ["--model", cfg.get("model", "sonnet")]
        self.proc = subprocess.Popen(args, cwd=workdir(), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, encoding="utf-8", bufsize=1,
                                     creationflags=_NO_WINDOW)

    def alive(self) -> bool:
        return self.proc.poll() is None

    def ask(self, question: str, on_text) -> str:
        """답 전체를 돌려준다. 실패하면 OSError."""
        message = {"type": "user", "message": {"role": "user", "content": question}}
        try:
            self.proc.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
            self.proc.stdin.flush()
        except (OSError, ValueError) as e:
            raise OSError(f"CLI 에 질문을 넣지 못했습니다: {e}") from e
        # 답이 멈추면 CLI 를 꺼서 아래 읽기를 끝낸다
        expired = threading.Event()

        def stop() -> None:
            expired.set()
            self.proc.kill()

        timer = threading.Timer(ANSWER_SECONDS, stop)
        timer.daemon = True
        timer.start()
        try:
            return self._read_answer(on_text)
        except OSError:
            if expired.is_set():
                try:
                    self.proc.wait(5)  # 다음 질문이 이 CLI 를 살아 있다고 보지 않게 끝날 때까지
                except subprocess.TimeoutExpired:
                    pass
                raise AnswerTimeout(f"답이 {ANSWER_SECONDS // 60}분 안에 오지 않았습니다") from None
            raise
        finally:
            timer.cancel()

    def _read_answer(self, on_text) -> str:
        text = ""
        for line in self.proc.stdout:
            try:
                event = json.loads(line)
            except ValueError:
                continue
            kind = event.get("type")
            if kind == "stream_event":
                delta = event.get("event", {}).get("delta", {})
                if delta.get("type") == "text_delta":
                    text += delta.get("text", "")
                    on_text(text)
            elif kind == "result":
                if event.get("is_error"):
                    raise OSError(str(event.get("result") or event.get("subtype") or "CLI 오류")[:200])
                return event.get("result") or text
        raise OSError("CLI 가 끝나 버렸습니다")

    def close(self) -> None:
        if self.alive():
            try:
                self.proc.stdin.close()
                self.proc.wait(5)
            except (OSError, subprocess.TimeoutExpired):
                self.proc.kill()


class _Take:
    """녹음 하나. 녹음이 끝나면 줄에 넣고 처리 스레드가 차례대로 받아 적는다 (녹음할 때의 설정을 같이 가진다)."""

    def __init__(self, dictate: bool = False, voice: bool = False, orca_pick: bool = False):
        self.dictate = dictate  # 받아쓰기 (Claude 에 묻지 않고 PC 에 입력만 한다)
        self.voice = voice  # 답을 소리로 읽어 줄지 (끝낼 때 다시 받는다)
        self.orca_pick = orca_pick  # ORCA 세션으로 가는 받아쓰기인지
        self.orca_target: str | None = None  # 그 세션의 터미널 handle (말하는 동안 따로 찾는다)
        self.chunks: list = []
        self.recording = threading.Event()  # 녹음마다 따로 둔다 (앞 녹음 스레드가 다음 녹음까지 이어 받지 않게)
        self.recording.set()
        self.recorder: threading.Thread | None = None
        self.failed = False  # 녹음이 실패해 _record 가 이미 알렸다

    @property
    def mode(self) -> str:
        return "dictate" if self.dictate else "ask"


class Assistant:
    def __init__(self, cfg: dict, model_dir: str = ""):
        self.cfg = cfg
        # 음성 인식 모델을 받아 두는 폴더 (설정의 [storage] model_dir). 비우면 Hugging Face 기본 자리(~/.cache/huggingface)
        self.model_dir = str(Path(os.path.expandvars(model_dir)).expanduser()) if model_dir else None
        self.idle_seconds = cfg.get("idle_minutes", 30) * 60
        self.command: tuple[str, str] | None = None
        self.session: Session | None = None
        self.model = None  # 음성 인식 모델 (처음 녹음할 때 불러온다)
        self.lock = threading.Lock()  # session / model 을 다루는 동안
        self.last_used = 0.0
        self.take: _Take | None = None  # 지금 녹음 중인 것
        self.takes: queue.Queue[_Take] = queue.Queue()  # 녹음을 마치고 처리를 기다리는 것
        self.busy = False  # 받아 적기·답 받기 중 (처리 스레드)
        threading.Thread(target=self._work, daemon=True).start()
        # PC 마이크를 쓸 수 있는지 (mic_available, _record 가 고친다). 기기는 false 면 말하기 버튼을 끈다.
        self.mic = True
        self.mic_reason = ""
        self._mic_failed_until = 0.0
        # 다른 소리 스트림(기기 마이크 -> 가상 오디오 장치)이 열려 있는지. 열려 있으면 장치 목록을 다시 읽지 않는다
        # (PortAudio 를 다시 켜면 열린 스트림이 끊긴다). main.py 가 정해 준다.
        self.other_audio = lambda: False
        # 마이크 입력 통로. 처음 여는 데 10초 넘게 걸릴 때가 있어서 한 번 열어 두고 다시 쓴다
        # (녹음하지 않는 동안은 멈춰 둔다. 멈춘 동안은 마이크를 쓰지 않는다). 오래 안 쓰면 닫는다.
        self._stream = None
        self._stream_name = ""  # 그 통로의 마이크 이름
        self._opened = False  # 이 도우미에서 마이크를 한 번이라도 열었는지 (_refresh_devices)
        self._unmatched = ""  # 장치 목록을 다시 읽어도 찾지 못한 기본 마이크 이름 (또 다시 읽지 않는다)
        # 장치 목록을 새로 읽으려고 도우미를 다시 띄울 까닭과 다시 띄우는 함수 (main.restart, main.py 가 넣어 준다)
        self.restart_for: str | None = None
        self.restart = None
        # 답을 소리로 읽어 줄지 (기기 질문 화면의 음소거 버튼, 기본은 음소거). PC 음량은 건드리지 않는다.
        self.speaker = speech.Speaker(cfg)
        # 녹음하는 동안 나온 답은 마이크에 들어가지 않게 녹음이 끝난 뒤 읽는다
        self.spoken_later: str | None = None
        # ORCA 세션에 받아쓰기할 때 쓴다 (main.py 가 넣어 준다)
        self.orca: orca.Orca | None = None
        self.pending_toast: str | None = None  # main.py 가 기기 알림으로 보낸다
        self.state: dict | None = None  # 기기로 보낼 최신 처리 상태 (받아 적는 중·답·오류)
        self.rec_state: dict | None = None  # 녹음 중이면 그 상태 (준비 중·듣는 중). 처리 상태보다 먼저 보낸다
        self.sent: dict | None = None
        self.sent_at = 0.0

    # --- 기기 -> PC ---

    def on_event(self, event: dict) -> None:
        action = event.get("ask")
        if action in ("start", "cancel", "new", "mute"):
            # 새로 말하기 시작하면 읽던 답은 끊는다 (마이크에 PC 소리가 들어가지 않게)
            self.speaker.stop()
        if action in ("start", "cancel", "new", "mute"):
            self.spoken_later = None
        if action == "start":
            if self.recording:
                # 녹음 중에 또 누르면 무시한다 (받아쓰기/질문 구분이 바뀌면 안 된다)
                print("[assistant] 녹음 중이라 새로 누른 것은 무시합니다")
                self.sent = None  # 기기에 지금 상태를 다시 보낸다
                return
            if self.takes.qsize() >= MAX_QUEUED_TAKES:
                print(f"[assistant] 처리를 기다리는 녹음이 {MAX_QUEUED_TAKES}개라 새로 누른 것은 받지 않습니다")
                self.pending_toast = f"앞의 녹음 {MAX_QUEUED_TAKES}개를 처리하는 중입니다\n잠시 뒤 다시 눌러 주세요"
                self.sent = None
                return
            dictate = event.get("mode") == "dictate"
            target = event.get("orca") if dictate else None
            # voice: 60초가 넘어 스스로 끝낼 때 쓴다 (끝낼 때 다시 받는다)
            take = _Take(dictate, bool(event.get("voice")), isinstance(target, str) and self.orca is not None)
            if take.orca_pick:
                # 말하는 동안 그 세션 탭으로 바꿔 둔다 (끝나면 그 세션에 넣는다)
                def activate() -> None:
                    take.orca_target = self.orca.activate(target)
                threading.Thread(target=activate, daemon=True).start()
            self._start(take)
        elif action == "stop":
            if self.take:
                self.take.voice = bool(event.get("voice"))
            self._stop()
        elif action == "mute":
            for take in [self.take, *list(self.takes.queue)]:
                if take:
                    take.voice = False
        elif action == "cancel":
            self._end_take()
            if not self.busy and self.takes.empty():
                self._set("idle")
        elif action == "new":
            self._end_take()
            with self.lock:
                if self.session:
                    self.session.close()
                    self.session = None
            print("[assistant] 새 대화")
            self._set("idle", a="새 대화를 시작합니다")

    @property
    def recording(self) -> bool:
        take = self.take
        return take is not None and take.recording.is_set()

    def _start(self, take: _Take) -> None:
        if self.recording:
            return
        self.take = take
        self.last_used = time.monotonic()  # 녹음을 취소해도 열어 둔 마이크·모델을 나중에 내리게
        # 마이크가 실제로 소리를 받기 시작하면 _record 가 "listening" 으로 바꾼다 (그때부터 말하면 된다)
        self._set_rec(take, "preparing")
        take.recorder = threading.Thread(target=self._record, args=(take,), daemon=True)
        take.recorder.start()
        # 녹음하는 동안 모델과 CLI 를 미리 준비해 둔다 (처음이면 몇 초 걸린다)
        threading.Thread(target=self._warm, args=(take,), daemon=True).start()

    def _end_take(self, take: _Take | None = None) -> None:
        """녹음을 끝낸다 (줄에 넣지는 않는다). take 를 주면 그것이 지금 녹음일 때만. 미뤄 둔 답 읽기를 이제 읽는다."""
        current = self.take
        if current is None or (take is not None and take is not current):
            return
        current.recording.clear()
        self.take = None
        self.rec_state = None
        later, self.spoken_later = self.spoken_later, None
        if later:
            self.speaker.speak(later)

    # --- PC 마이크 ---

    # 녹음하는 동안 잡고 있는다. 그동안은 장치 목록을 다시 읽지 않는다 (열린 녹음이 끊긴다)
    _audio_lock = threading.Lock()

    def mic_available(self) -> tuple[bool, str]:
        """(PC 마이크를 쓸 수 있는지, 못 쓰면 그 이유). main.py 가 30초마다 부른다. self.mic / self.mic_reason 도 바꾼다."""
        if not self._audio_lock.acquire(blocking=False):
            return self.mic, self.mic_reason  # 녹음 중이면 마이크가 있다
        try:
            self.mic, self.mic_reason = self._check_mic(refresh=True)
        finally:
            self._audio_lock.release()
        return self.mic, self.mic_reason

    def _check_mic(self, refresh: bool) -> tuple[bool, str]:
        import sounddevice as sd

        # 방금 열지 못한 마이크는 잠시 없는 것으로 둔다 (장치는 보이는데 열리지 않을 때 버튼이 켜졌다 꺼졌다 하지 않게)
        if time.monotonic() < getattr(self, "_mic_failed_until", 0.0):
            return False, self.mic_reason
        if self._mic_privacy_blocked():
            return False, "Windows 가 마이크를 막았습니다\n설정 > 개인 정보 > 마이크에서\n데스크톱 앱 허용을 켜 주세요"
        # 마이크가 없던 때만 다시 읽어 새로 꽂은 것을 찾는다. 있던 마이크가 빠지면 녹음할 때 실패로 알게 된다.
        if refresh and not self.mic and self._stream is None:
            # PortAudio 는 처음 켤 때 본 장치만 기억한다. 나중에 꽂거나 뺀 마이크를 알도록 다시 읽는다
            self._refresh_devices(sd, "마이크 없음")
        name = self.cfg.get("input") or None
        try:
            info = sd.query_devices(name, kind="input")
        except Exception:  # 기본 마이크가 없거나 config 의 이름과 맞는 마이크가 없다
            return False, f"'{name}' 마이크를 찾지 못했습니다" if name else "PC 에 마이크가 없습니다"
        if info["max_input_channels"] < 1:
            return False, "PC 에 마이크가 없습니다"
        return True, ""

    @staticmethod
    def _mic_privacy_blocked() -> bool:
        """Windows 설정 > 개인 정보 > 마이크 에서 막혀 있는지. 막혀 있으면 녹음은 열려도 소리가 0 으로만 들어온다."""
        if sys.platform != "win32":
            return False
        import winreg

        key = r"Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\microphone"
        # 이 장치 전체, 이 사용자 전체, 데스크톱 앱(도우미는 여기에 든다) 순서
        for root, sub in ((winreg.HKEY_LOCAL_MACHINE, key), (winreg.HKEY_CURRENT_USER, key),
                          (winreg.HKEY_CURRENT_USER, key + r"\NonPackaged")):
            try:
                with winreg.OpenKey(root, sub) as k:
                    if str(winreg.QueryValueEx(k, "Value")[0]).lower() == "deny":
                        return True
            except OSError:
                continue
        return False

    def _record(self, take: _Take) -> None:
        import numpy as np
        import sounddevice as sd

        def fail(reason: str, no_mic: bool) -> None:
            print(f"[assistant] 녹음 실패: {reason}".replace("\n", " "))
            take.failed = True
            take.chunks = []
            self._end_take(take)
            if no_mic:
                self.mic, self.mic_reason = False, reason
            if self.busy or not self.takes.empty():
                # 앞 녹음의 답을 받는 중이면 그 화면을 덮지 않고 알림으로
                self.pending_toast = reason
            else:
                self._set("error", a=reason, mode=take.mode)

        if not self._audio_lock.acquire(timeout=3):
            # 앞 녹음의 마이크가 아직 닫히지 않았다
            fail("마이크가 아직 닫히는 중입니다\n잠시 뒤 다시 눌러 주세요", False)
            return
        with _Held(self._audio_lock):
            ok, reason = self._check_mic(refresh=not self.mic)  # 없던 마이크를 방금 꽂았을 수 있다
            if not ok:
                fail(reason, True)
                return
            self.mic, self.mic_reason = True, ""
            order = self._mic_order(sd)
            silent: list[str] = []  # 열렸지만 소리가 오지 않은 마이크
            error = None  # 열지 못한 마이크의 마지막 오류
            lid = False  # 덮개가 닫혀 내장 마이크가 0 만 보냈다
            for retry in (False, True):
                if retry:
                    if silent or not take.recording.is_set():
                        break
                    # 하나도 열지 못했다. PortAudio 는 처음 켤 때 본 장치만 기억해서, 그 뒤 덮개를 여닫거나
                    # 에어팟·아이폰 마이크가 붙고 빠지면 열리지 않는다 (Mac: Invalid Property Value). 목록을 다시 읽는다
                    print(f"[assistant] 마이크를 열지 못했습니다: {str(error)[:80]}")
                    self._close_stream()
                    if not self._refresh_devices(sd, "마이크를 열지 못함"):
                        break
                    order = self._mic_order(sd)
                for i, name in enumerate(order):
                    if not take.recording.is_set():
                        return
                    if i and order[i - 1] in silent:
                        # 앞 마이크가 조용했다: 다음 마이크로 처음부터 다시 녹음한다
                        self.pending_toast = f"'{order[i - 1][:16]}' 마이크가 조용해서\n'{name[:16]}' 로 바꿉니다"
                        self._set_rec(take, "preparing")
                        take.chunks = []
                    try:
                        if self._stream is None or self._stream_name != name:
                            self._close_stream()
                            opened = time.monotonic()
                            self._stream = self._open_stream(sd, name)
                            self._stream_name = name
                            print(f"[assistant] 마이크를 열었습니다 ({name}, {time.monotonic() - opened:.1f}초)")
                        stream = self._stream
                        stream.start()
                    except Exception as e:  # 장치는 보이는데 열리지 않을 때 (다른 프로그램이 독점, 드라이버 문제 등)
                        print(f"[assistant] '{name}' 마이크를 열지 못했습니다: {str(e)[:80]}")
                        self._close_stream()
                        error = e
                        continue
                    quiet = False
                    try:
                        if take.recording.is_set():
                            self._set_rec(take, "listening")
                        start = time.monotonic()
                        heard = False
                        while take.recording.is_set() and time.monotonic() - start < MAX_RECORD_SECONDS:
                            # 기다리지 않고 읽는다: 마이크 권한을 묻는 중이거나 막혀 있으면 소리가 아예 오지 않아
                            # read() 가 멈춰 버리고, 그러면 아무 말 없이 빈 녹음이 된다
                            if stream.read_available < SAMPLE_RATE // 10:
                                if not take.chunks and time.monotonic() - start > 2.0:
                                    quiet = True
                                    break
                                time.sleep(0.02)
                                continue
                            data, _ = stream.read(SAMPLE_RATE // 10)
                            take.chunks.append(data.copy())
                            heard = heard or bool(np.any(data))
                            # 진짜 마이크는 조용해도 잡음이 있다. 1.5초 동안 완전히 0 이면 음소거됐거나 막힌 것이다
                            if not heard and time.monotonic() - start > 1.5:
                                # 외장 마이크라면 덮개와 상관없다
                                lid = lid or ("macbook" in name.lower() and _lid_closed())
                                quiet = True
                                break
                        if not quiet and take.recording.is_set():
                            # 끝 버튼을 잊었다: 60초에서 스스로 끝내고 받아 적는다
                            print(f"[assistant] {MAX_RECORD_SECONDS}초가 지나 녹음을 끝냅니다")
                            threading.Thread(target=self._stop, daemon=True).start()
                    finally:
                        self._halt(stream)
                    if not quiet:
                        return
                    print(f"[assistant] '{name}' 에서 소리가 오지 않습니다")
                    silent.append(name)
                    # 조용한 마이크는 닫는다 (다음 녹음은 다시 시스템 기본 마이크부터)
                    self._close_stream()
            if silent:
                reason = _LID_CLOSED_REASON if lid and len(silent) == 1 else _NO_AUDIO_REASON
                if len(silent) > 1:
                    reason = f"마이크 {len(silent)}개를 모두 써 봤지만\n" + reason
                fail(reason, False)
            elif take.recording.is_set():
                self._mic_failed_until = time.monotonic() + 300
                fail(f"PC 마이크를 열지 못했습니다\n{str(error or '마이크가 없습니다')[:60]}", True)

    def _mic_order(self, sd) -> list[str]:
        """녹음할 마이크 순서: config 의 input (없으면 시스템 기본 마이크), 그다음 이 PC 의 다른 마이크 모두."""
        def input_names() -> list[str]:
            devices = [d for d in sd.query_devices() if d["max_input_channels"] > 0]
            try:
                hostapi = sd.query_devices(kind="input")["hostapi"]
                devices = [d for d in devices if d["hostapi"] == hostapi] or devices
            except Exception:
                pass
            return [str(d["name"]) for d in devices]

        names = input_names()
        wanted = self.cfg.get("input", "")
        if wanted:
            first = next((n for n in names if wanted.lower() in n.lower()), None)
        else:
            full = _system_default_input()
            first = _match_input(full, names) if full else None
            if full and first is None and full != self._unmatched:
                # 도우미를 켠 뒤 연결된 마이크다
                if self._refresh_devices(sd, f"기본 마이크 '{full}'"):
                    names = input_names()
                    first = _match_input(full, names)
                    if first is None:
                        # 다시 읽어도 없다 (이름이 PortAudio 와 다르게 나온다). 녹음마다 다시 읽지 않는다
                        print(f"[assistant] 기본 마이크 '{full}' 를 장치 목록에서 찾지 못해 기본 장치로 엽니다")
                        self._unmatched = full
            if first is None:
                try:
                    first = str(sd.query_devices(kind="input")["name"])
                except Exception:
                    first = None
        order = [first] if first else []
        # 진짜 기본 마이크를 앞에 두었으면 그것을 가리키기만 하는 별칭 장치는 또 시도하지 않는다
        real = bool(first) and not any(a in first.lower() for a in _ALIASES)
        for name in names:
            lower = name.lower()
            if name in order or any(v in lower for v in _VIRTUAL) or (real and any(a in lower for a in _ALIASES)):
                continue
            order.append(name)
        return order

    def _open_stream(self, sd, name: str):
        index = _input_index(sd, name)
        if index is None:  # None 을 넘기면 PortAudio 가 다른 마이크(처음 본 기본 마이크)를 연다
            raise OSError(f"'{name}' 마이크가 장치 목록에 없습니다")
        stream = sd.InputStream(samplerate=SAMPLE_RATE, channels=1, dtype="int16", device=index)
        self._opened = True
        return stream

    def _refresh_devices(self, sd, why: str) -> bool:
        """PortAudio 의 장치 목록을 새로 읽는다. 읽었으면 True.
        마이크를 한 번이라도 연 뒤에 다시 읽으면 도우미가 끝날 때까지 어느 마이크에서도 소리가 오지 않는다
        (Mac 에서 확인). 그때는 바로 읽지 않고, 녹음하지 않는 틈에 도우미를 다시 띄운다 (pending).
        Windows 는 그런 일을 본 적이 없고 다시 띄우면 트레이 앱이 도우미를 놓치므로 전처럼 바로 읽는다."""
        if self.other_audio():  # 다시 읽으면 열린 소리 스트림이 끊긴다
            return False
        if not self._opened or sys.platform != "darwin":
            self._close_stream()
            try:
                sd._terminate()
                sd._initialize()
                return True
            except Exception as e:
                print(f"[assistant] 오디오 장치 목록을 다시 읽지 못했습니다: {e}")
                return False
        # 같은 까닭으로 다시 띄운 지 얼마 안 됐으면 또 띄우지 않는다 (다시 띄워도 그대로인 장치)
        at, _, before = os.environ.get(_RESTARTED_ENV, "").partition("|")
        if before == why and time.time() - float(at or 0) < RESTART_AGAIN_SECONDS:
            return False
        if self.restart_for is None:
            print(f"[assistant] 장치 목록을 새로 읽어야 합니다 ({why}). 녹음하지 않는 틈에 도우미를 다시 띄웁니다")
            self.restart_for = why
        return False

    def _halt(self, stream) -> None:
        """녹음을 멈춘다. Mac 에서 stream.stop() 이 돌아오지 않아 다음 녹음이 모두 막힌 적이 있다.
        abort() 는 쌓인 소리도 버린다. 1초 안에 안 끝나면 그 스트림은 버리고 다음에 새로 연다."""
        done = threading.Event()

        def abort() -> None:
            try:
                stream.abort()
            except Exception:
                pass
            done.set()

        threading.Thread(target=abort, daemon=True).start()
        if not done.wait(1.0):
            print("[assistant] 마이크가 바로 멈추지 않아 다음에는 새로 엽니다")
            self._stream = None

    def _close_stream(self) -> None:
        stream, self._stream = self._stream, None
        if stream is not None:
            try:
                stream.close()
            except Exception:
                pass

    def _stop(self) -> None:
        """녹음을 끝내고 줄에 넣는다. 처리는 _work 가 차례대로 한다 (그동안 새로 녹음할 수 있다)."""
        take = self.take
        if take is None or not take.recording.is_set():
            return
        if not self.busy and self.takes.empty():
            self._set("thinking", a="받아 적는 중", mode=take.mode)
        self.takes.put(take)
        self._end_take(take)

    # --- 받아 적기와 묻기 ---

    def _work(self) -> None:
        """처리 스레드: 녹음을 마친 순서대로 받아 적고 묻는다 (CLI 대화가 질문 순서대로 이어지게 하나씩)."""
        while True:
            take = self.takes.get()
            self.busy = True
            try:
                self._answer(take)
            finally:
                self.busy = False

    def _warm(self, take: _Take) -> None:
        try:
            with self.lock:
                if not self._chatgpt():  # ChatGPT 로 받아 적으면 Whisper 는 실패할 때만 불러온다
                    self._load_model()
                if not take.dictate:  # 받아쓰기는 Claude 를 쓰지 않는다
                    self._open_session()
        except Exception as e:
            print(f"[assistant] 준비 실패: {e}")

    def _chatgpt(self) -> bool:
        return self.cfg.get("stt", "local") == "chatgpt"

    def _language(self) -> str | None:
        """Whisper 에 줄 언어. "auto" 면 None (Whisper 가 알아본다)."""
        language = self.cfg.get("language", "ko")
        return None if language == "auto" else language

    def _load_model(self) -> None:
        if self.model is not None:
            return
        name = self.cfg.get("whisper_model", "small")
        if self.cfg.get("engine", "auto") in ("auto", "mlx") and name in MLX_MODELS and _mlx_ready():
            print(f"[assistant] 음성 인식 모델 불러오는 중 ({name}, GPU)")
            loading = time.monotonic()
            try:
                self.model = _MlxModel(MLX_MODELS[name], self.model_dir, self._language())
                print(f"[assistant] 음성 인식 모델을 불러왔습니다 ({time.monotonic() - loading:.1f}초)")
                return
            except Exception as e:
                print(f"[assistant] GPU 모델을 불러오지 못해 CPU 로 받아 적습니다: {e}")
        from faster_whisper import WhisperModel

        # 회사망은 Hugging Face 에서 모델을 받지 못할 수 있다. 다른 PC 에서 받아 복사해 둔 폴더가 있으면 그것을 쓴다
        # (Mac 에서 companion/windows/pack_model.py 로 만든다)
        local = Path(self.model_dir or Path.home() / ".llm-deck" / "models") / f"faster-whisper-{name}"
        if Path(name).expanduser().is_dir():
            name = str(Path(name).expanduser())
        elif (local / "model.bin").is_file():
            name = str(local)
        print(f"[assistant] 음성 인식 모델 불러오는 중 ({name})")
        loading = time.monotonic()
        try:
            try:
                # 받아 둔 모델이 있으면 인터넷에 묻지 않는다 (Hugging Face 확인이 응답 없이 멈춘 적이 있다)
                self.model = WhisperModel(name, device="cpu", compute_type="int8", download_root=self.model_dir,
                                          local_files_only=True)
            except Exception:
                self.model = WhisperModel(name, device="cpu", compute_type="int8", download_root=self.model_dir)
            print(f"[assistant] 음성 인식 모델을 불러왔습니다 ({time.monotonic() - loading:.1f}초)")
        except Exception as e:
            print(f"[assistant] 음성 인식 모델을 불러오지 못했습니다 ({name}): {e}")
            print(f"[assistant] 인터넷으로 받을 수 없으면, 다른 PC 에서 받은 모델 폴더를 {local} 에 복사하세요 (README.txt 참고)")
            raise OSError("음성 인식 모델을 불러오지 못했습니다\n(기록 파일 참고)") from e

    def _open_session(self) -> None:
        if self.session and self.session.alive():
            return
        if self.command is None:
            self.command = find_command(self.cfg)
            if self.command is None:
                raise OSError("Claude CLI 를 찾지 못했습니다 (claude-test, claude)")
        print(f"[assistant] {self.command[0]} 를 백그라운드에 띄웁니다")
        self.session = Session(self.command[1], self.cfg)

    def _answer(self, take: _Take) -> None:
        import numpy as np

        mode = take.mode
        try:
            if take.recorder:
                take.recorder.join(2)
            if not take.chunks:
                # 녹음이 실패했으면 _record 가 이미 오류를 보냈다. 아니면 너무 짧게 눌렀다
                if not take.failed:
                    print("[assistant] 녹음된 소리가 없습니다 (너무 짧게 눌렀거나 마이크가 열리지 않음)")
                    self._set("error", a="소리가 녹음되지 않았습니다\n버튼을 누르고 말한 뒤\n다시 눌러 주세요", mode=mode)
                elif self.state and self.state["state"] == "thinking" and not self.state["q"]:
                    # 끝을 누를 때 넣어 둔 "받아 적는 중" 을 지운다 (오류는 이미 알림으로 나갔다)
                    self._set("idle", mode=mode)
                return
            pcm = np.concatenate(take.chunks)[:, 0].astype(np.float32) / 32768
            peak = float(np.abs(pcm).max())
            print(f"[assistant] 녹음 {len(pcm) / SAMPLE_RATE:.1f}초, 가장 큰 소리 {peak:.3f}")
            self._set("thinking", a="받아 적는 중", mode=mode)
            waited = time.monotonic()
            with self.lock:
                start = time.monotonic()
                question = None
                engine = "ChatGPT"
                if not _has_speech(pcm):
                    # 어느 엔진이든 말이 없는 녹음에서는 없는 말을 지어낸다
                    question, engine = "", "말소리 없음"
                elif self._chatgpt():
                    question = chatgpt_stt.text(pcm, SAMPLE_RATE, self.cfg.get("language", "ko"))
                if question is None:
                    engine = "Whisper"
                    self._load_model()
                    if time.monotonic() - waited > 2:
                        print(f"[assistant] 모델 준비를 {time.monotonic() - waited:.1f}초 기다렸습니다")
                    start = time.monotonic()
                    language = self._language()
                    # ORCA 세션에 받아쓰기할 때는 / 명령 이름을 영어로 알아듣게 돕는다
                    hints = self.orca.hotwords() if take.dictate and take.orca_pick and self.orca else None
                    prompt = self.cfg.get("prompt", DEFAULT_PROMPT) or None
                    if isinstance(self.model, _MlxModel):
                        # mlx-whisper 는 hotwords 가 없어서 앞 문맥에 붙인다. 앞 문맥이 길면 앞쪽이 잘리므로
                        # 자주 쓰는 말이 남도록 / 명령 이름을 앞에 둔다
                        question = self.model.text(pcm, language, " ".join(p for p in (hints, prompt) if p) or None)
                    else:
                        segments, _ = self.model.transcribe(pcm, language=language,
                                                            beam_size=self.cfg.get("beam_size", 1), vad_filter=True,
                                                            initial_prompt=prompt, hotwords=hints)
                        question = "".join(s.text for s in segments
                                           if _heard(s.no_speech_prob, s.avg_logprob)).strip()
                kind = "받아쓰기" if take.dictate else "질문"
                print(f"[assistant] {kind} ({engine}, {len(pcm) / SAMPLE_RATE:.1f}초 녹음, "
                      f"{time.monotonic() - start:.1f}초): {question}")
                if _drop_made_up(question) != question:
                    print(f"[assistant] 조용한 소리에서 지어낸 말로 보여 버립니다: {question}")
                    question = ""
                if not question:
                    hint = "마이크 가까이에서\n다시 말해 주세요" if peak < QUIET_PEAK else "다시 말해 주세요"
                    if take.dictate:
                        self._set("idle", mode="dictate")
                        self.pending_toast = f"알아듣지 못했습니다\n{hint}"
                    else:
                        self._set("error", a=f"알아듣지 못했습니다\n{hint}")
                    return
                if take.dictate:
                    self._finish_dictation(take, question)
                    return
                self._set("thinking", q=question, a="")
                self._open_session()
                start = time.monotonic()
                try:
                    answer = self.session.ask(question, lambda text: self._set("thinking", q=question, a=text))
                except OSError:
                    # CLI 가 죽어 있었으면 한 번만 새로 띄워 다시 묻는다 (앞 대화는 잊는다)
                    self.session.close()
                    self.session = None
                    self._open_session()
                    answer = self.session.ask(question, lambda text: self._set("thinking", q=question, a=text))
                print(f"[assistant] 답 ({time.monotonic() - start:.1f}초): {answer[:80]}")
                self._set("done", q=question, a=answer)
                if take.voice:
                    if self.recording:
                        self.spoken_later = spoken_text(question, answer)
                    else:
                        self.speaker.speak(spoken_text(question, answer))
        except Exception as e:
            print(f"[assistant] 실패: {e}")
            self._set("error", a=f"답을 받지 못했습니다\n{str(e)[:80]}", mode=mode)
        finally:
            # 받아쓰기만 했거나 묻다가 실패해도 모델·CLI·마이크는 올라와 있으므로, 오래 안 쓰면 내리도록 시각을 남긴다
            self.last_used = time.monotonic()

    def _finish_dictation(self, take: _Take, text: str) -> None:
        """받아 적은 글을 PC 의 커서 자리에 입력한다. 결과는 기기 알림으로. (빈 글은 부르는 쪽이 먼저 걸러 낸다)"""
        if self.orca:
            fixed = orca.fix_slash(text, self.orca.commands(), self.orca.aliases)
            if fixed != text:
                print(f"[assistant] / 명령으로 바꿈: {fixed}")
            text = fixed
        target = take.orca_target
        if target:
            error = self.orca.send_text(target, text)
            where = "ORCA에 "
        elif take.orca_pick:
            # 고른 세션이 그사이 닫혔으면 엉뚱한 창에 치지 않는다
            error, where = "ORCA 세션을 찾지 못했습니다", ""
        else:
            error = typer._type(text, enter=False)
            where = ""
        self._set("idle", mode="dictate")
        self.pending_toast = f"{where}입력했습니다: {text[:40]}" if not error else f"입력하지 못했습니다\n{error}"

    # --- PC -> 기기 ---

    def _set(self, state: str, q: str = "", a: str = "", mode: str = "ask") -> None:
        """처리 상태 (받아 적는 중·답·오류). 받아쓰기는 mode="dictate"."""
        busy = state == "thinking"
        # 받아쓰기는 질문 화면의 질문·답을 바꾸지 않는다 (기기는 mode 를 보고 사용량 화면 버튼에만 보여 준다)
        self.state = {"state": state, "q": q[:200], "a": a[:MAX_ANSWER_CHARS], "done": not busy, "mode": mode}

    def _set_rec(self, take: _Take, state: str) -> None:
        """녹음 상태 (preparing, listening). 지금 녹음 중인 것만 (끝난 녹음의 스레드가 늦게 바꾸지 않게)."""
        if take is self.take and take.recording.is_set():
            self.rec_state = {"state": state, "q": "", "a": "", "done": False, "mode": take.mode}

    def pending(self) -> dict | None:
        """기기로 보낼 것 (바뀌었을 때만). main.py 가 0.5초마다 부른다. 오래 안 쓰면 CLI 를 내린다."""
        self._unload_if_idle()
        self._restart_if_wanted()
        state = self._outgoing()
        if state is None or state == self.sent:
            return None
        # 답이 나오는 중에는 너무 자주 보내지 않는다 (끝·상태 바뀜은 바로)
        partial = self.sent and state["state"] == self.sent["state"] == "thinking" and state["q"] == self.sent["q"]
        if partial and time.monotonic() - self.sent_at < PARTIAL_SECONDS:
            return None
        self.sent, self.sent_at = state, time.monotonic()
        return state

    def _outgoing(self) -> dict | None:
        """기기에 보일 상태. 녹음 중이면 녹음 상태 (기기는 녹음 상태가 아닌 것을 받으면 녹음 버튼을 푼다).
        그동안 앞 녹음의 답은 PC 에서 계속 받아 두고, 녹음이 끝나면 보낸다."""
        if self.rec_state:
            return self.rec_state
        waiting = self.takes.qsize()
        return {**self.state, "queued": waiting} if self.state and waiting else self.state

    def _restart_if_wanted(self) -> None:
        """장치 목록을 새로 읽으려고 도우미를 다시 띄운다. 녹음·받아 적기 중이 아니고, 결과를 기기에 보낼 틈이 지났을 때."""
        if not self.restart_for or self.restart is None or self.busy or self.recording or not self.takes.empty():
            return
        if self._outgoing() != self.sent or self.pending_toast or time.monotonic() - self.last_used < 5:
            return
        os.environ[_RESTARTED_ENV] = f"{time.time():.0f}|{self.restart_for}"
        print(f"[assistant] 장치 목록을 새로 읽으려고 도우미를 다시 띄웁니다 ({self.restart_for})")
        self.restart()

    def _unload_if_idle(self) -> None:
        if not self.last_used or self.busy or self.recording or not self.takes.empty():
            return
        if time.monotonic() - self.last_used < self.idle_seconds:
            return
        if not self.lock.acquire(blocking=False):
            return
        try:
            if self.session:
                self.session.close()
                self.session = None
            self.model = None
            if self._audio_lock.acquire(blocking=False):
                try:
                    self._close_stream()
                finally:
                    self._audio_lock.release()
            self.last_used = 0.0
            print(f"[assistant] {self.idle_seconds // 60}분 동안 질문이 없어 CLI 를 내렸습니다")
        finally:
            self.lock.release()

    def close(self) -> None:
        # 녹음 중인 쪽이 마이크를 멈추는 동안 닫으면 macOS 오디오가 서로 기다리며 멈춘다 (PortAudio). 끝날 때까지 기다린다.
        self._end_take()
        self.spoken_later = None
        self.speaker.stop()
        with self._audio_lock:
            self._close_stream()
        with self.lock:
            if self.session:
                self.session.close()
                self.session = None
