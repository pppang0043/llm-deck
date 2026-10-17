"""기기에서 말로 묻고 글로 답받기 (Claude Code CLI 를 구독 계정으로 쓴다).

흐름: 기기 {"ask": "start"} -> 이 PC 마이크로 녹음 -> {"ask": "stop"} -> 이 PC 안에서 글자로 바꾸기(faster-whisper)
      -> 미리 띄워 둔 CLI 에 질문 -> 답을 나오는 대로 기기로 보낸다 {"ask": {"state", "q", "a", "done"}}
      {"ask": "cancel"} 녹음 버리기, {"ask": "new"} 새 대화 (앞 대화를 잊는다)
      {"ask": "stop", "voice": true} 면 답이 다 나온 뒤 이 PC 스피커로 읽어 준다 (speech.py). {"ask": "mute"} 읽기 멈춤
      읽는 것은 답의 앞 1~2문장(요약)이다. 질문에 "길게", "자세히" 같은 말이 있으면 답 전체를 읽는다.
받아쓰기: {"ask": "start", "mode": "dictate"} ... {"ask": "stop"} -> 받아 적은 글을 이 PC 의 커서 자리에 입력한다
      (Claude 에 묻지 않는다, Enter 는 누르지 않는다). 기기 사용량 화면의 버튼이 쓴다.

CLI 는 소리를 받지 못하므로 음성은 먼저 글자로 바꾼다. 소리는 인터넷으로 나가지 않는다.
CLI 는 질문마다 새로 켜면 2~3초가 더 걸려서, 한 번 띄워 두고(stream-json) 계속 묻는다. 같은 프로세스라 대화도 이어진다.
30분 동안 질문이 없으면 CLI 와 음성 인식 모델을 내려 메모리를 돌려준다 (다음 질문 때 다시 띄운다).
회사 PC 는 명령 이름이 다를 수 있어서 claude-test 를 먼저 찾고, 없으면 claude 를 쓴다.
"""

import json
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

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
# 기기 화면에 보낼 답의 최대 글자 수 (넘으면 뒤를 자른다)
MAX_ANSWER_CHARS = 1500
# 답이 나오는 동안 기기로 보내는 간격 (BLE 로도 따라갈 수 있게)
PARTIAL_SECONDS = 0.5


# 로그인 때 자동으로 켜진 도우미는 PATH 가 짧아서(터미널과 다르다) CLI 가 깔리는 자리를 직접 더 찾는다
EXTRA_DIRS = [
    "~/.local/bin", "~/.claude/local", "~/.npm-global/bin", "/opt/homebrew/bin", "/usr/local/bin",
    "%APPDATA%\\npm", "%USERPROFILE%\\.local\\bin",
]


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


class Assistant:
    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.idle_seconds = cfg.get("idle_minutes", 30) * 60
        self.command: tuple[str, str] | None = None
        self.session: Session | None = None
        self.model = None  # 음성 인식 모델 (처음 녹음할 때 불러온다)
        self.lock = threading.Lock()  # session / model 을 다루는 동안
        self.last_used = 0.0
        self.recording = threading.Event()
        self.chunks: list = []
        self.recorder: threading.Thread | None = None
        self.busy = False  # 받아 적기·답 받기 중
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
        # 답을 소리로 읽어 줄지 (기기 질문 화면의 음소거 버튼, 기본은 음소거). PC 음량은 건드리지 않는다.
        self.voice = False
        self.speaker = speech.Speaker(cfg)
        # 이번 녹음이 받아쓰기인지 (사용량 화면 버튼). 받아쓰기는 Claude 에 묻지 않고 PC 에 입력만 한다.
        self.dictate = False
        self.pending_toast: str | None = None  # main.py 가 기기 알림으로 보낸다
        self.state: dict | None = None  # 기기로 보낼 최신 상태
        self.sent: dict | None = None
        self.sent_at = 0.0

    # --- 기기 -> PC ---

    def on_event(self, event: dict) -> None:
        action = event.get("ask")
        if action in ("start", "cancel", "new", "mute"):
            # 새로 말하기 시작하면 읽던 답은 끊는다 (마이크에 PC 소리가 들어가지 않게)
            self.speaker.stop()
        if action == "start":
            self.dictate = event.get("mode") == "dictate"
            self._start()
        elif action == "stop":
            self.voice = bool(event.get("voice"))
            self._stop()
        elif action == "mute":
            self.voice = False
        elif action == "cancel":
            self.recording.clear()
            self._set("idle")
        elif action == "new":
            self.recording.clear()
            with self.lock:
                if self.session:
                    self.session.close()
                    self.session = None
            print("[assistant] 새 대화")
            self._set("idle", a="새 대화를 시작합니다")

    def _start(self) -> None:
        if self.busy or self.recording.is_set():
            return
        self.chunks = []
        self.recording.set()
        self.recorder = threading.Thread(target=self._record, daemon=True)
        self.recorder.start()
        # 마이크가 실제로 소리를 받기 시작하면 _record 가 "listening" 으로 바꾼다 (그때부터 말하면 된다)
        self._set("preparing")
        # 녹음하는 동안 모델과 CLI 를 미리 준비해 둔다 (처음이면 몇 초 걸린다)
        threading.Thread(target=self._warm, daemon=True).start()

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
        if refresh and not self.mic and not self.other_audio() and self._stream is None:
            # PortAudio 는 처음 켤 때 본 장치만 기억한다. 나중에 꽂거나 뺀 마이크를 알도록 다시 읽는다
            try:
                sd._terminate()
                sd._initialize()
            except Exception as e:
                print(f"[assistant] 오디오 장치 목록을 다시 읽지 못했습니다: {e}")
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

    def _record(self) -> None:
        import numpy as np
        import sounddevice as sd

        def fail(reason: str, no_mic: bool) -> None:
            print(f"[assistant] 녹음 실패: {reason}".replace("\n", " "))
            self.recording.clear()
            self.chunks = []
            if no_mic:
                self.mic, self.mic_reason = False, reason
            self._set("error", a=reason)

        with self._audio_lock:
            ok, reason = self._check_mic(refresh=not self.mic)  # 없던 마이크를 방금 꽂았을 수 있다
            if not ok:
                fail(reason, True)
                return
            self.mic, self.mic_reason = True, ""
            try:
                if self._stream is None:
                    opened = time.monotonic()
                    device = self.cfg.get("input") or None
                    self._stream = sd.InputStream(samplerate=SAMPLE_RATE, channels=1, dtype="int16", device=device)
                    print(f"[assistant] 마이크를 열었습니다 ({time.monotonic() - opened:.1f}초)")
                stream = self._stream
                stream.start()
                try:
                    if self.recording.is_set():
                        self._set("listening")
                    start = time.monotonic()
                    heard = False
                    while self.recording.is_set() and time.monotonic() - start < MAX_RECORD_SECONDS:
                        # 기다리지 않고 읽는다: 마이크 권한을 묻는 중이거나 막혀 있으면 소리가 아예 오지 않아
                        # read() 가 멈춰 버리고, 그러면 아무 말 없이 빈 녹음이 된다
                        if stream.read_available < SAMPLE_RATE // 10:
                            if not self.chunks and time.monotonic() - start > 2.0:
                                fail(_NO_AUDIO_REASON, False)
                                return
                            time.sleep(0.02)
                            continue
                        data, _ = stream.read(SAMPLE_RATE // 10)
                        self.chunks.append(data.copy())
                        heard = heard or bool(np.any(data))
                        # 진짜 마이크는 조용해도 잡음이 있다. 1.5초 동안 완전히 0 이면 음소거됐거나 막힌 것이다
                        if not heard and time.monotonic() - start > 1.5:
                            fail("마이크 소리가 들어오지 않습니다\n음소거나 마이크 권한을\n확인해 주세요", False)
                            return
                finally:
                    stream.stop()
                    # 멈춘 동안 쌓인 소리는 버린다 (다음 녹음에 앞 소리가 섞이지 않게)
                    while stream.read_available:
                        stream.read(stream.read_available)
            except Exception as e:  # 장치는 보이는데 열리지 않을 때 (다른 프로그램이 독점, 드라이버 문제 등)
                self._close_stream()
                self._mic_failed_until = time.monotonic() + 300
                fail(f"PC 마이크를 열지 못했습니다\n{str(e)[:60]}", True)

    def _close_stream(self) -> None:
        stream, self._stream = self._stream, None
        if stream is not None:
            try:
                stream.close()
            except Exception:
                pass

    def _stop(self) -> None:
        if not self.recording.is_set():
            return
        self.recording.clear()
        self.busy = True
        threading.Thread(target=self._answer, daemon=True).start()

    # --- 받아 적기와 묻기 ---

    def _warm(self) -> None:
        try:
            with self.lock:
                self._load_model()
                if not self.dictate:  # 받아쓰기는 Claude 를 쓰지 않는다
                    self._open_session()
        except Exception as e:
            print(f"[assistant] 준비 실패: {e}")

    def _load_model(self) -> None:
        if self.model is not None:
            return
        from faster_whisper import WhisperModel

        name = self.cfg.get("whisper_model", "small")
        # 회사망은 Hugging Face 에서 모델을 받지 못할 수 있다. 다른 PC 에서 받아 복사해 둔 폴더가 있으면 그것을 쓴다
        # (Mac 에서 companion/windows/pack_model.py 로 만든다)
        local = Path.home() / ".llm-deck" / "models" / f"faster-whisper-{name}"
        if Path(name).expanduser().is_dir():
            name = str(Path(name).expanduser())
        elif (local / "model.bin").is_file():
            name = str(local)
        print(f"[assistant] 음성 인식 모델 불러오는 중 ({name})")
        try:
            self.model = WhisperModel(name, device="cpu", compute_type="int8")
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

    def _answer(self) -> None:
        import numpy as np

        try:
            if self.recorder:
                self.recorder.join(2)
            if not self.chunks:
                # 녹음이 실패했으면 _record 가 이미 오류를 보냈다. 아니면 너무 짧게 눌렀다
                if self.state is None or self.state.get("state") != "error":
                    print("[assistant] 녹음된 소리가 없습니다 (너무 짧게 눌렀거나 마이크가 열리지 않음)")
                    self._set("error", a="소리가 녹음되지 않았습니다\n버튼을 누른 채 말해 주세요")
                return
            pcm = np.concatenate(self.chunks)[:, 0].astype(np.float32) / 32768
            self._set("thinking", a="받아 적는 중")
            with self.lock:
                self._load_model()
                start = time.monotonic()
                segments, _ = self.model.transcribe(pcm, language=self.cfg.get("language", "ko"), beam_size=1,
                                                    vad_filter=True)
                question = "".join(s.text for s in segments).strip()
                kind = "받아쓰기" if self.dictate else "질문"
                print(f"[assistant] {kind} ({len(pcm) / SAMPLE_RATE:.1f}초 녹음, {time.monotonic() - start:.1f}초): {question}")
                if self.dictate:
                    self._finish_dictation(question)
                    return
                if not question:
                    self._set("error", a="알아듣지 못했습니다. 다시 말해 주세요")
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
                if self.voice:
                    self.speaker.speak(spoken_text(question, answer))
                self.last_used = time.monotonic()
        except Exception as e:
            print(f"[assistant] 실패: {e}")
            self._set("error", a=f"답을 받지 못했습니다\n{str(e)[:80]}")
        finally:
            self.busy = False

    def _finish_dictation(self, text: str) -> None:
        """받아 적은 글을 PC 의 커서 자리에 입력한다. 결과는 기기 알림으로."""
        if not text:
            self._set("idle", mode="dictate")
            self.pending_toast = "알아듣지 못했습니다\n다시 말해 주세요"
            return
        error = typer._type(text, enter=False)
        self._set("idle", mode="dictate")
        self.pending_toast = f"입력했습니다: {text[:40]}" if not error else f"입력하지 못했습니다\n{error}"

    # --- PC -> 기기 ---

    def _set(self, state: str, q: str = "", a: str = "", mode: str | None = None) -> None:
        busy = state in ("preparing", "listening", "thinking")
        mode = mode or ("dictate" if self.dictate else "ask")
        # 받아쓰기는 질문 화면의 질문·답을 바꾸지 않는다 (기기는 mode 를 보고 사용량 화면 버튼에만 보여 준다)
        self.state = {"state": state, "q": q[:200], "a": a[:MAX_ANSWER_CHARS], "done": not busy, "mode": mode}

    def pending(self) -> dict | None:
        """기기로 보낼 것 (바뀌었을 때만). main.py 가 0.5초마다 부른다. 오래 안 쓰면 CLI 를 내린다."""
        self._unload_if_idle()
        state = self.state
        if state is None or state == self.sent:
            return None
        # 답이 나오는 중에는 너무 자주 보내지 않는다 (끝·상태 바뀜은 바로)
        partial = self.sent and state["state"] == self.sent["state"] == "thinking" and state["q"] == self.sent["q"]
        if partial and time.monotonic() - self.sent_at < PARTIAL_SECONDS:
            return None
        self.sent, self.sent_at = state, time.monotonic()
        return state

    def _unload_if_idle(self) -> None:
        if not self.last_used or self.busy or self.recording.is_set():
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
        self.recording.clear()
        self.speaker.stop()
        with self._audio_lock:
            self._close_stream()
        with self.lock:
            if self.session:
                self.session.close()
                self.session = None
