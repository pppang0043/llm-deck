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

import base64
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
# 소리가 오지 않은 마이크는 이만큼 동안 맨 뒤로 미룬다 (녹음마다 1.5초씩 버리고 "바꿉니다" 알림이 뜨지 않게)
QUIET_MIC_SECONDS = 600
# 앞 마이크가 조용해서 대신 녹음한 마이크. 그 마이크가 빠질 때까지 맨 앞에 두고 쓴다 (도우미를 다시 켜도)
FALLBACK_MIC_FILE = Path.home() / ".llm-deck" / "fallback_mic.txt"
# 기기에서 고른 마이크 (비어 있으면 자동). 그 마이크가 있으면 늘 먼저 쓴다
PICKED_MIC_FILE = Path.home() / ".llm-deck" / "picked_mic.txt"
# 쉬는 틈에 마이크를 미리 들어 보는 횟수 (모두 조용하면 PROBE_GAP 초 뒤 다시). 그래도 안 되면 "연결 불가"
PROBE_TRIES = 3
PROBE_GAP = 3
# 연결 불가일 때 다시 들어 보는 간격 (덮개를 열었을 때처럼 장치 목록은 그대로인데 되살아나는 때)
RECHECK_SECONDS = 60
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
_PRIVACY_REASON = "Windows 가 마이크를 막았습니다\n설정 > 개인 정보 > 마이크에서\n데스크톱 앱 허용을 켜 주세요"
# 소리가 늘 0 인 가상 장치. 마이크를 돌아가며 잡을 때 건너뛴다
_VIRTUAL = ("blackhole", "cable output", "zoomaudio", "teams audio", "soundflower")
# Windows 의 "기본 마이크" 별칭 장치 (MME 사운드 매퍼, DirectSound 주 캡처 드라이버). 진짜 기본 마이크를 알면 건너뛴다
_ALIASES = ("sound mapper", "사운드 매퍼", "primary sound capture", "주 사운드 캡처")
# Mac 은 블루투스 이어폰 마이크를 쓰지 않는다. 쓰는 순간 음악용(AAC) 연결이 통화용(HFP)으로 바뀌어 듣는 소리가 나빠지고,
# 바뀌는 1~3초 동안은 0 만 와서 녹음이 실패했다. 기본 마이크가 에어팟이면 이 PC 의 다른 마이크(MacBook 마이크)로 녹음한다.
# 연결 방식(_mac_inputs)으로 알아보고, 이름은 그것을 못 읽을 때만 본다. Windows 는 시스템에서 고른 마이크를 그대로 따른다
_BLUETOOTH = ("airpods", "bluetooth", "beats", "buds", "hands-free", "handsfree", "핸즈프리")


def _bluetooth(name: str) -> bool:
    return any(b in name.lower() for b in _BLUETOOTH)


def _system_default_input() -> str | None:
    """시스템 설정에서 지금 고른 기본 마이크 이름. 못 읽으면 None.
    PortAudio 는 처음 켤 때의 기본 마이크만 기억해서 (micproc.py) 직접 묻는다."""
    if sys.platform == "darwin":
        return _mac_default_input()
    if sys.platform == "win32":
        return _windows_default_input()
    return None


def _fourcc(text: str) -> int:
    return int.from_bytes(text.encode(), "big")


def _coreaudio():
    """Mac CoreAudio 함수들과 속성 주소 구조체."""
    import ctypes
    import ctypes.util
    from ctypes import POINTER, Structure, c_char_p, c_long, c_uint32, c_void_p

    class Address(Structure):
        _fields_ = [("selector", c_uint32), ("scope", c_uint32), ("element", c_uint32)]

    ca = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreAudio"))
    cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
    ca.AudioObjectGetPropertyDataSize.argtypes = [c_uint32, POINTER(Address), c_uint32, c_void_p, POINTER(c_uint32)]
    ca.AudioObjectGetPropertyData.argtypes = [c_uint32, POINTER(Address), c_uint32, c_void_p,
                                              POINTER(c_uint32), c_void_p]
    ca.AudioObjectSetPropertyData.argtypes = [c_uint32, POINTER(Address), c_uint32, c_void_p, c_uint32, c_void_p]
    cf.CFStringGetCString.argtypes = [c_void_p, c_char_p, c_long, c_uint32]
    cf.CFRelease.argtypes = [c_void_p]
    return ca, cf, Address


def _mac_uint(ca, Address, obj: int, selector: str, scope: str = "glob") -> int | None:
    from ctypes import byref, c_uint32, sizeof

    value, size = c_uint32(0), c_uint32(sizeof(c_uint32))
    if ca.AudioObjectGetPropertyData(obj, byref(Address(_fourcc(selector), _fourcc(scope), 0)), 0, None,
                                     byref(size), byref(value)):
        return None
    return value.value


def _mac_name(ca, cf, Address, device: int) -> str | None:
    import ctypes
    from ctypes import byref, c_uint32, c_void_p, sizeof

    name, size = c_void_p(), c_uint32(sizeof(c_void_p))
    if ca.AudioObjectGetPropertyData(device, byref(Address(_fourcc("lnam"), _fourcc("glob"), 0)), 0, None,
                                     byref(size), byref(name)) or not name.value:
        return None
    buf = ctypes.create_string_buffer(512)
    ok = cf.CFStringGetCString(name, buf, 512, 0x08000100)  # UTF-8
    cf.CFRelease(name)
    return buf.value.decode() if ok else None


def _mac_default_input() -> str | None:
    """Mac: 시스템 설정 > 사운드 > 입력 에서 고른 장치 (CoreAudio)."""
    try:
        ca, cf, Address = _coreaudio()
        device = _mac_uint(ca, Address, 1, "dIn ")  # 시스템(1)의 기본 입력 장치
        return _mac_name(ca, cf, Address, device) if device else None
    except Exception:
        return None


def _mac_inputs() -> dict[str, tuple[int, str]]:
    """Mac 의 마이크들: 이름 -> (장치 번호, 연결 방식 'bltn' 내장, 'blue'·'blea' 블루투스, 'usb ' ...). 못 읽으면 {}.
    블루투스인지는 이름이 아니라 연결 방식으로 안다 (이름을 바꾼 에어팟, 'Galaxy Buds' 같은 다른 이어폰도)."""
    from ctypes import byref, c_uint32, sizeof

    try:
        ca, cf, Address = _coreaudio()
        size = c_uint32(0)
        everything = Address(_fourcc("dev#"), _fourcc("glob"), 0)
        if ca.AudioObjectGetPropertyDataSize(1, byref(everything), 0, None, byref(size)):
            return {}
        devices = (c_uint32 * (size.value // sizeof(c_uint32)))()
        if ca.AudioObjectGetPropertyData(1, byref(everything), 0, None, byref(size), devices):
            return {}
        found = {}
        for device in devices:
            streams = c_uint32(0)
            if ca.AudioObjectGetPropertyDataSize(device, byref(Address(_fourcc("stm#"), _fourcc("inpt"), 0)), 0, None,
                                                 byref(streams)) or not streams.value:
                continue  # 입력이 없는 장치 (스피커)
            name = _mac_name(ca, cf, Address, device)
            kind = _mac_uint(ca, Address, device, "tran") or 0
            if name:
                found[name] = (int(device), kind.to_bytes(4, "big").decode("latin-1"))
        return found
    except Exception:
        return {}


_MAC_BLUETOOTH = ("blue", "blea")


def _mac_use_builtin_input() -> str | None:
    """Mac: 기본 입력이 블루투스 이어폰(에어팟)이면 내장 마이크로 되돌린다. 되돌렸으면 원래 장치 이름.
    에어팟이 다시 붙으면 macOS 가 기본 입력까지 에어팟으로 바꾸는데, 그대로 두면 ORCA·브라우저처럼 기본 마이크를 쓰는
    앱이 마이크를 여는 순간 에어팟이 통화용(HFP)으로 바뀌어 소리가 나빠진다."""
    from ctypes import byref, c_uint32, sizeof

    name = _mac_default_input()
    inputs = _mac_inputs()
    if not name or not (inputs.get(name, (0, ""))[1] in _MAC_BLUETOOTH or _bluetooth(name)):
        return None
    builtin = next((device for device, kind in inputs.values() if kind == "bltn"), None)  # MacBook 마이크
    if builtin is None:
        return None
    try:
        ca, _, Address = _coreaudio()
        if ca.AudioObjectSetPropertyData(1, byref(Address(_fourcc("dIn "), _fourcc("glob"), 0)), 0, None,
                                         sizeof(c_uint32), byref(c_uint32(builtin))):
            return None
    except Exception:
        return None
    return name


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


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def _write_text(path: Path, text: str) -> None:
    """빈 글이면 파일을 지운다."""
    try:
        if text:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        else:
            path.unlink(missing_ok=True)
    except OSError as e:
        print(f"[assistant] {path.name} 에 적지 못했습니다: {e}")


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


class _MicProcess:
    """micproc.py 를 자식 프로세스로 띄운다. 받은 줄은 on_message 로 넘긴다."""

    def __init__(self, on_message):
        self.proc = subprocess.Popen([sys.executable, "-u", str(Path(__file__).with_name("micproc.py"))],
                                     cwd=Path(__file__).parent, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1,
                                     creationflags=_NO_WINDOW)

        def read() -> None:
            for line in self.proc.stdout:
                try:
                    on_message(json.loads(line))
                except ValueError:
                    continue

        def errors() -> None:
            for line in self.proc.stderr:
                print(f"[mic] {line.rstrip()}")

        threading.Thread(target=read, daemon=True).start()
        threading.Thread(target=errors, daemon=True).start()

    def alive(self) -> bool:
        return self.proc.poll() is None

    def send(self, cmd: dict) -> None:
        try:
            self.proc.stdin.write(json.dumps(cmd, ensure_ascii=False) + "\n")
            self.proc.stdin.flush()
        except (OSError, ValueError) as e:
            raise OSError(f"녹음 프로세스에 보내지 못했습니다: {e}") from e

    def close(self) -> None:
        self.proc.kill()


class MicClient:
    """마이크는 녹음 프로세스(micproc.py)에서만 연다. 장치 목록을 새로 읽거나 멈춘 마이크를 버릴 때는
    그 프로세스만 끝내고 다음에 부를 때 새로 띄운다 (도우미는 그대로 돈다)."""

    def __init__(self, spawn=_MicProcess):
        self._spawn = spawn
        self._proc = None
        self._replies: queue.Queue[dict] = queue.Queue()
        self._audio: queue.Queue[str] = queue.Queue()
        self.spawned = 0  # 띄운 횟수

    def _on_message(self, message: dict) -> None:
        if "pcm" in message:
            self._audio.put(message["pcm"])
        else:
            self._replies.put(message)

    def _ask(self, cmd: dict, timeout: float) -> dict:
        if self._proc is None or not self._proc.alive():
            self._proc = self._spawn(self._on_message)
            self.spawned += 1
        while not self._replies.empty():  # 앞에서 늦게 온 답 (읽다가 난 오류 등)
            self._replies.get_nowait()
        self._proc.send(cmd)
        try:
            return self._replies.get(timeout=timeout)
        except queue.Empty:
            print(f"[assistant] 녹음 프로세스가 {timeout:.0f}초 동안 답하지 않아 새로 띄웁니다 ({cmd['op']})")
            self.stop()
            raise OSError("마이크가 응답하지 않습니다") from None

    def devices(self) -> tuple[list[str], str]:
        """(마이크 이름들, PortAudio 의 기본 마이크). 녹음 프로세스를 띄운 때의 목록이다."""
        reply = self._ask({"op": "devices"}, 20)
        return list(reply.get("devices", [])), str(reply.get("default", ""))

    def listen(self, name: str) -> float:
        """그 마이크로 듣기 시작한다. 여는 데 걸린 초. 열지 못하면 OSError."""
        while not self._audio.empty():
            self._audio.get_nowait()
        reply = self._ask({"op": "listen", "name": name}, 20)  # 처음 열 때 10초 넘게 걸린 적이 있다
        if "opened" not in reply:
            raise OSError(reply.get("error") or "마이크를 열지 못했습니다")
        return float(reply.get("secs", 0))

    def read(self, timeout: float):
        """들은 소리 한 조각 (int16, (n, 1)). 아직 없으면 None."""
        import numpy as np

        try:
            data = self._audio.get(timeout=timeout)
        except queue.Empty:
            return None
        return np.frombuffer(base64.b64decode(data), dtype=np.int16).reshape(-1, 1).copy()

    def halt(self) -> None:
        """듣기를 멈춘다 (마이크는 열어 둔다). 멈추지 않으면 녹음 프로세스를 끝낸다."""
        if self._proc is not None and self._proc.alive():
            try:
                self._ask({"op": "halt"}, 3)
            except OSError:
                pass

    def restart(self) -> None:
        """장치 목록을 새로 읽도록 다음에 부를 때 녹음 프로세스를 새로 띄운다."""
        self.stop()

    def stop(self) -> None:
        proc, self._proc = self._proc, None
        if proc is not None:
            proc.close()


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
        self.mic = ""  # 녹음한 마이크 이름

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
        # PC 마이크 상태 (기기 상태줄의 마이크 표시, mic_status): "checking" 들어 보는 중, "ok" 쓸 수 있음, "none" 연결 불가.
        # self.mic 이 False 면 기기는 말하기 버튼을 끈다. 녹음과 미리 들어 보기(_probe_round)가 고친다.
        self.mic_state = "checking"
        self.mic_name = ""  # 소리가 오는 것을 확인한 마이크
        self.mic = True
        self.mic_reason = ""
        # 마이크는 녹음 프로세스에서 연다 (micproc.py). 오래 안 쓰면 끈다
        self.mics = MicClient()
        self._names: list[str] = []  # 고를 수 있는 마이크 (기기에 보낸다)
        self._unmatched = ""  # 장치 목록을 다시 읽어도 찾지 못한 기본 마이크 이름 (또 다시 읽지 않는다)
        self._quiet_until: dict[str, float] = {}  # 소리가 오지 않은 마이크 -> 다시 먼저 써 볼 시각 (monotonic)
        self._fallback = _read_text(FALLBACK_MIC_FILE)
        self._picked = _read_text(PICKED_MIC_FILE)
        self._bt_names: set[str] = set()  # Mac 의 블루투스 마이크 (_device_signature 가 채운다)
        # 마이크 장치가 바뀌었는지 2초마다 본다 (_watch_mics). 바뀌면 쉬는 틈에 미리 들어 본다
        self._watched = 0.0
        self._signature = None
        self._fresh = True  # 녹음 프로세스가 지금 장치 목록을 알고 있는지 (아니면 들어 보기 전에 새로 띄운다)
        self._check_due: float | None = 0.0  # 다음에 미리 들어 볼 시각 (None 이면 장치가 바뀔 때까지 안 한다)
        self._tries = 0  # 이어서 모두 조용했던 횟수
        self._probing = False
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
        if isinstance(event.get("mic_pick"), str):
            self._pick(event["mic_pick"][:120])
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

    def mic_status(self) -> dict:
        """기기 상태줄의 마이크 표시와 마이크 고르기 창에 보낼 것 {"pc_mic": ...}"""
        return {"state": self.mic_state, "name": self.mic_name[:40], "why": self.mic_reason[:120],
                "list": [n[:40] for n in self._names[:8]], "pick": self._picked[:40]}

    def _set_mic(self, state: str, name: str = "", why: str = "") -> None:
        if state == "none" and self.mic_state != "none":
            print(f"[assistant] PC 마이크 연결 불가: {why}".replace("\n", " "))
        elif state == "ok" and (self.mic_state, self.mic_name) != ("ok", name):
            print(f"[assistant] PC 마이크: {name}")
        self.mic_state, self.mic_name = state, name
        self.mic, self.mic_reason = state != "none", why

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

        def fail(reason: str, no_mic: bool) -> None:
            print(f"[assistant] 녹음 실패: {reason}".replace("\n", " "))
            take.failed = True
            take.chunks = []
            self._end_take(take)
            if no_mic:
                self._set_mic("none", why=reason)
                self._check_due = time.monotonic() + RECHECK_SECONDS
            if self.busy or not self.takes.empty():
                # 앞 녹음의 답을 받는 중이면 그 화면을 덮지 않고 알림으로
                self.pending_toast = reason
            else:
                self._set("error", a=reason, mode=take.mode)

        # 미리 들어 보는 중이면 끝날 때까지 기다린다 (마이크마다 2초 안쪽, 처음 열 때는 더)
        if not self._audio_lock.acquire(timeout=10):
            fail("마이크가 아직 닫히는 중입니다\n잠시 뒤 다시 눌러 주세요", False)
            return
        with _Held(self._audio_lock):
            if self._mic_privacy_blocked():
                fail(_PRIVACY_REASON, True)
                return
            order = self._mic_order()
            silent: list[str] = []  # 열렸지만 소리가 오지 않은 마이크
            error = None  # 열지 못한 마이크의 마지막 오류
            lid = False  # 덮개가 닫혀 내장 마이크가 0 만 보냈다
            for retry in (False, True):
                if retry:
                    if silent or not take.recording.is_set():
                        break
                    # 하나도 열지 못했다. PortAudio 는 처음 켤 때 본 장치만 기억해서, 그 뒤 덮개를 여닫거나
                    # 에어팟·아이폰 마이크가 붙고 빠지면 열리지 않는다 (Mac: Invalid Property Value).
                    # 녹음 프로세스를 새로 띄워 목록을 다시 읽는다
                    print(f"[assistant] 마이크를 열지 못했습니다: {str(error)[:80]}")
                    self.mics.restart()
                    order = self._mic_order()
                for i, name in enumerate(order):
                    if not take.recording.is_set():
                        return
                    if i and order[i - 1] in silent:
                        # 앞 마이크가 조용했다: 다음 마이크로 처음부터 다시 녹음한다
                        self.pending_toast = f"'{order[i - 1][:16]}' 마이크가 조용해서\n'{name[:16]}' 로 바꿉니다"
                        self._set_rec(take, "preparing")
                        take.chunks = []
                    try:
                        secs = self.mics.listen(name)
                        if secs >= 1:
                            print(f"[assistant] 마이크를 열었습니다 ({name}, {secs:.1f}초)")
                    except OSError as e:  # 장치는 보이는데 열리지 않을 때 (다른 프로그램이 독점, 드라이버 문제 등)
                        print(f"[assistant] '{name}' 마이크를 열지 못했습니다: {str(e)[:80]}")
                        error = e
                        if name == self._fallback:
                            self._save_fallback("")
                        continue
                    quiet = False
                    try:
                        if take.recording.is_set():
                            self._set_rec(take, "listening")
                        start = time.monotonic()
                        heard = False
                        while take.recording.is_set() and time.monotonic() - start < MAX_RECORD_SECONDS:
                            data = self.mics.read(0.05)
                            if data is None:
                                # 마이크 권한을 묻는 중이거나 막혀 있으면 소리가 아예 오지 않는다
                                if not take.chunks and time.monotonic() - start > 2.0:
                                    quiet = True
                                    break
                                continue
                            take.chunks.append(data)
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
                        self.mics.halt()
                    if not quiet:
                        take.mic = name
                        self._mic_works(name, silent)
                        return
                    print(f"[assistant] '{name}' 에서 소리가 오지 않습니다")
                    self._mic_silent(name, silent)
            if silent:
                reason = _LID_CLOSED_REASON if lid and len(silent) == 1 else _NO_AUDIO_REASON
                if len(silent) > 1:
                    reason = f"마이크 {len(silent)}개를 모두 써 봤지만\n" + reason
                fail(reason, True)
            elif take.recording.is_set():
                fail(f"PC 마이크를 열지 못했습니다\n{str(error or '마이크가 없습니다')[:60]}", True)

    def _mic_works(self, name: str, silent: list[str]) -> None:
        """그 마이크에서 소리가 왔다. 앞 마이크가 조용해서 대신 쓴 것이면 빠질 때까지 먼저 쓴다."""
        self._quiet_until.pop(name, None)
        if silent and name != self._fallback:
            self._save_fallback(name)
        self._set_mic("ok", name)
        self._tries = 0
        self._check_due = None

    def _mic_silent(self, name: str, silent: list[str]) -> None:
        silent.append(name)
        self._quiet_until[name] = time.monotonic() + QUIET_MIC_SECONDS
        if name == self._fallback:
            self._save_fallback("")
        # 다시 붙은 에어팟처럼 녹음 프로세스가 예전 장치로 알고 있을 수 있다. 다음에 들어 볼 때는 새로 띄운다
        self._fresh = False

    def _bt(self, name: str) -> bool:
        """Mac 에서 쓰지 않는 블루투스 마이크인지. Windows 는 시스템에서 고른 마이크를 그대로 따른다."""
        return sys.platform == "darwin" and (name in self._bt_names or _bluetooth(name))

    def _mic_order(self) -> list[str]:
        """녹음할 마이크 순서: 기기에서 고른 마이크, 앞서 대신 쓴 마이크, config 의 input (없으면 시스템 기본 마이크),
        그다음 이 PC 의 다른 마이크 모두. Mac 은 블루투스 이어폰 마이크를 기본 마이크여도 뺀다 (_bt)."""
        try:
            names, default = self.mics.devices()
        except OSError as e:
            print(f"[assistant] 마이크 목록을 읽지 못했습니다: {e}")
            names, default = [], ""
        wanted = self.cfg.get("input", "")
        if wanted:
            first = next((n for n in names if wanted.lower() in n.lower()), None)
        else:
            full = _system_default_input()
            first = _match_input(full, names) if full else None
            if full and first is None and full != self._unmatched and not self._bt(full):
                # 녹음 프로세스를 띄운 뒤 연결된 마이크다 (에어팟이면 어차피 쓰지 않으니 다시 읽지 않는다)
                print(f"[assistant] 새 기본 마이크 '{full}' 를 찾으려고 녹음 프로세스를 새로 띄웁니다")
                self.mics.restart()
                try:
                    names, default = self.mics.devices()
                except OSError:
                    names, default = [], ""
                first = _match_input(full, names)
                if first is None:
                    # 다시 읽어도 없다 (이름이 PortAudio 와 다르게 나온다). 녹음마다 다시 읽지 않는다
                    print(f"[assistant] 기본 마이크 '{full}' 를 장치 목록에서 찾지 못해 기본 장치로 엽니다")
                    self._unmatched = full
            first = first or default or None
        order = [first] if first else []
        # 진짜 기본 마이크를 앞에 두었으면 그것을 가리키기만 하는 별칭 장치는 또 시도하지 않는다
        real = bool(first) and not any(a in first.lower() for a in _ALIASES)
        for name in names:
            lower = name.lower()
            if name in order or any(v in lower for v in _VIRTUAL) or (real and any(a in lower for a in _ALIASES)):
                continue
            order.append(name)
        order = [n for n in order if not self._bt(n)]
        self._names = [n for n in order if not any(a in n.lower() for a in _ALIASES)] or order
        if self._fallback and self._fallback not in order:
            print(f"[assistant] 대신 쓰던 '{self._fallback}' 마이크가 빠져 기본 마이크로 돌아갑니다")
            self._save_fallback("")
        # 고른 마이크, 앞서 기본 마이크가 조용해서 대신 쓴 마이크 순서로 앞에 둔다 (녹음마다 바꾼다는 알림이 뜨지 않게)
        for name in (self._fallback, self._picked):
            if name in order:
                order.remove(name)
                order.insert(0, name)
        # 얼마 전 소리가 오지 않은 마이크는 맨 뒤로 (다른 마이크도 모두 조용할 때만 써 본다)
        now = time.monotonic()
        quiet = [n for n in order if self._quiet_until.get(n, 0.0) > now]
        return [n for n in order if n not in quiet] + quiet

    def _save_fallback(self, name: str) -> None:
        """대신 쓸 마이크를 정한다 (빈 글이면 지운다). 도우미를 다시 켜도 이어 쓰게 파일에 적는다."""
        self._fallback = name
        if name:
            print(f"[assistant] 앞으로 '{name}' 마이크를 먼저 씁니다 (빠질 때까지)")
        _write_text(FALLBACK_MIC_FILE, name)

    def _pick(self, name: str) -> None:
        """기기의 마이크 고르기 창에서 고른 마이크 (빈 글이면 자동). 바로 다시 들어 보고 상태를 알린다."""
        if name == self._picked:
            return
        print(f"[assistant] 기기에서 마이크를 골랐습니다: {name or '자동'}")
        self._picked = name
        _write_text(PICKED_MIC_FILE, name)
        if name:
            self._quiet_until.pop(name, None)
        self._tries = 0
        self._check_due = time.monotonic()
        self._watched = 0.0  # 2초를 기다리지 않고 바로 들어 본다

    # --- 쉬는 틈에 마이크 미리 들어 보기 ---

    def _device_signature(self) -> tuple:
        """마이크 장치가 바뀌었는지 알아보는 값. Mac 은 마이크 목록과 기본 마이크, Windows 는 기본 마이크."""
        if sys.platform == "darwin":
            inputs = _mac_inputs()
            self._bt_names = {n for n, (_, kind) in inputs.items() if kind in _MAC_BLUETOOTH}
            return _mac_default_input(), tuple(sorted(inputs))
        return (_system_default_input(),)

    def _watch_mics(self) -> None:
        """2초마다: Mac 기본 입력이 에어팟이면 MacBook 마이크로 되돌리고, 마이크 장치가 바뀌었거나 다시 볼 때가 되었으면
        녹음하지 않는 틈에 미리 들어 본다 (_probe_round). 그러면 녹음 버튼을 누르자마자 소리가 오는 마이크로 녹음한다."""
        now = time.monotonic()
        if now - self._watched < 2:
            return
        self._watched = now
        if sys.platform == "darwin":
            moved = _mac_use_builtin_input()
            if moved:
                print(f"[assistant] 기본 입력이 '{moved}' 로 바뀌어 MacBook 마이크로 되돌렸습니다")
        signature = self._device_signature()
        if signature != self._signature:
            if self._signature is not None:
                print("[assistant] 마이크 장치가 바뀌어 다시 들어 봅니다")
                self._fresh = False
            self._signature = signature
            self._tries = 0
            self._check_due = now
        if self._check_due is None or now < self._check_due or self._probing or self.recording:
            return
        self._probing = True
        threading.Thread(target=self._probe_round, daemon=True).start()

    def _probe_round(self) -> None:
        """마이크를 순서대로 잠깐 들어 보고 소리가 오는 첫 마이크를 쓸 마이크로 정한다.
        모두 조용하면 PROBE_GAP 초 뒤 다시, PROBE_TRIES 번이면 연결 불가로 알리고 RECHECK_SECONDS 마다 다시 본다."""
        try:
            if not self._audio_lock.acquire(blocking=False):  # 녹음 중
                self._check_due = time.monotonic() + PROBE_GAP
                return
            with _Held(self._audio_lock):
                if self._mic_privacy_blocked():
                    self._set_mic("none", why=_PRIVACY_REASON)
                    self._check_due = time.monotonic() + RECHECK_SECONDS
                    return
                if self._tries == 0 and self.mic_state != "none":
                    self._set_mic("checking")
                if not self._fresh:
                    self.mics.restart()
                    self._fresh = True
                order = self._mic_order()
                silent: list[str] = []
                lid = False
                for name in order:
                    if self.take is not None:  # 녹음을 시작했다. 녹음이 마이크 상태를 정한다
                        return
                    heard = self._probe(name)
                    if heard:
                        self._mic_works(name, silent)
                        return
                    if heard is False:
                        lid = lid or ("macbook" in name.lower() and _lid_closed())
                        self._mic_silent(name, silent)
                self._tries += 1
                if self._tries < PROBE_TRIES:
                    print(f"[assistant] 소리가 오는 마이크가 없어 {PROBE_GAP}초 뒤 다시 들어 봅니다 ({self._tries}/{PROBE_TRIES})")
                    self._check_due = time.monotonic() + PROBE_GAP
                    return
                if not order:
                    why = "PC 에 마이크가 없습니다"
                elif not silent:
                    why = "PC 마이크를 열지 못했습니다"
                else:
                    why = _LID_CLOSED_REASON if lid and len(silent) == 1 else _NO_AUDIO_REASON
                self._set_mic("none", why=why)
                self._check_due = time.monotonic() + RECHECK_SECONDS
        finally:
            self._probing = False

    def _probe(self, name: str) -> bool | None:
        """잠깐 들어 본다: 소리가 오면 True, 조용하면 False, 열지 못하면 None. 소리가 오면 바로 멈춘다."""
        import numpy as np

        try:
            self.mics.listen(name)
        except OSError as e:
            print(f"[assistant] '{name}' 마이크를 열지 못했습니다: {str(e)[:80]}")
            return None
        try:
            start = time.monotonic()
            while time.monotonic() - start < 2.0:
                data = self.mics.read(0.05)
                if data is None:
                    continue
                if np.any(data):
                    return True
                if time.monotonic() - start > 1.5:
                    break
            return False
        finally:
            self.mics.halt()

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
                    hint = "다시 말해 주세요"
                    if peak < QUIET_PEAK:
                        # 소리가 너무 작았다: 어느 마이크인지 알려 주고 바꿀 곳을 안내한다
                        hint = (f"'{take.mic[:16]}' 소리가 작습니다\n가까이에서 말하거나 위의\n마이크 표시를 눌러 바꾸세요"
                                if take.mic else "마이크 가까이에서\n다시 말해 주세요")
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
        self._watch_mics()
        self._unload_if_idle()
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
                    self.mics.stop()  # 녹음 프로세스를 끈다 (다음 녹음 때 다시 띄운다)
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
            self.mics.stop()
        with self.lock:
            if self.session:
                self.session.close()
                self.session = None
