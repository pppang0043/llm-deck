"""ORCA 에 열린 세션들의 작업 상태를 기기로 보내고, 기기에서 고른 세션에 받아쓰기한 글을 입력한다.

  - 10초마다 orca CLI 로 세션 목록과 상태를 읽는다 (orca worktree ps, orca terminal list). 바뀌었을 때만 기기로 보낸다.
      PC -> 기기: {"orca": {"ok": bool, "list": [{"id", "t": 이름, "p": 프로젝트, "s": 상태}]}}
      s: working(작업 중) / waiting(입력 기다림) / permission(권한 요청) / blocked / done / idle / shell(AI 없는 터미널)
  - 기기에서 세션을 누르면 {"ask": "start", "mode": "dictate", "orca": id}: 그 세션 탭으로 바꾸고 ORCA 를 앞으로 띄운다.
    받아 적은 글은 커서 자리가 아니라 그 세션 터미널에 직접 넣는다 (orca terminal send). Enter 는 누르지 않는다.
  - 기기에서 세션을 길게 누르면 빠른 답: {"orca": "reply", "id": id, "reply": "allow" | "deny" | "continue"}.
    정해 둔 답(REPLIES)만 보내고, 보내기 직전에 상태를 다시 읽어 그 답이 맞는 상태일 때만 보낸다.
  - 받아쓰기에서 "슬래시 커밋" 처럼 말하면 "/commit" 으로 바꾼다 (Claude Code 의 / 명령·스킬). slash_words 참고.
ORCA 가 꺼져 있거나 orca 명령을 못 찾으면 {"ok": false} 를 보낸다.
"""

import difflib
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import threading
import time
from pathlib import Path

import typer

POLL_SECONDS = 10
MAX_SESSIONS = 24  # 기기 orcapanel.cpp 의 MAX_ROWS
RECENT_FILE = Path.home() / ".llm-deck" / "orca_recent.json"
_NO_WINDOW = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
BUNDLE_ID = "com.stablyai.orca"  # Mac 의 ORCA 앱
# ORCA 화면 세션 팝업의 키 (이름 -> 알림에 쓰는 이름, 보낼 글, 끝에 Enter). 나머지 이름은 typer.KEYWORDS 의 글 + Enter.
# Tab + Enter 는 Claude Code 가 흐리게 보여준 추천 문장을 받아서 바로 보낸다.
KEYS = {"tab": ("Tab", "\t", False), "enter": ("Enter", "", True), "tab_enter": ("Tab + Enter", "\t", True)}
# 빠른 답 (이름 -> 알림에 쓰는 이름, 보낼 글, 끝에 Enter, 보내도 되는 상태). 기기는 이름만 보낸다.
# Claude Code 권한 묻기는 "1. Yes / 2. ... / 3. No (esc)" 고르기라 허용은 "1" (Yes 를 바로 고른다), 거부는 Esc.
# 허용에 Enter 를 쓰지 않는다: 목록이 늦어 이미 입력칸으로 돌아갔으면 "1" 만 남고 아무것도 보내지지 않는다.
REPLIES = {
    "allow": ("허용", "1", False, {"permission"}),
    "deny": ("거부", "\x1b", False, {"permission"}),
    "continue": ("계속", "계속", True, {"waiting", "awaiting-input", "done"}),
}

# 로그인 때 켜진 도우미는 PATH 가 짧아서 orca 명령이 깔리는 자리를 직접 더 찾는다
EXTRA_PATHS = [
    "/Applications/Orca.app/Contents/Resources/bin/orca",
    "~/Applications/Orca.app/Contents/Resources/bin/orca",
    r"%LOCALAPPDATA%\Programs\orca\resources\bin\orca.exe",
]


def _native(path: str) -> str:
    # Windows 의 orca.cmd 는 cmd.exe 를 거치며 & | % ^ 나 줄바꿈이 든 글을 망가뜨리거나 명령으로 실행한다.
    # 옆의 orca.exe 를 바로 부른다 (orca.cmd 도 그렇게 하라고 적혀 있다)
    if path.lower().endswith((".cmd", ".bat")):
        exe = os.path.splitext(path)[0] + ".exe"
        if os.path.exists(exe):
            return exe
    return path


def find_orca() -> str | None:
    path = shutil.which("orca")
    if path:
        return _native(path)
    for candidate in EXTRA_PATHS:
        path = os.path.expanduser(os.path.expandvars(candidate))
        if os.path.exists(path):
            return path
    if sys.platform == "darwin":
        # 다운로드한 자리에서 바로 연 ORCA 는 macOS 가 임시 경로로 옮겨 실행한다. 지금 떠 있는 ORCA 에서 찾는다
        try:
            out = subprocess.run(["ps", "-axo", "comm="], capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.TimeoutExpired):
            return None
        for line in out.splitlines():
            if line.endswith("/Orca.app/Contents/MacOS/Orca"):
                path = line[: -len("MacOS/Orca")] + "Resources/bin/orca"
                if os.path.exists(path):
                    return path
    return None


# --- "슬래시" 를 / 명령으로 ---

SLASH_WORD = re.compile(r"(?:슬래시|슬래쉬|슬레시|슬레쉬|슬러시|슬래 시|slash)\s*[,.]?\s*", re.IGNORECASE)
# Claude Code 에 들어 있는 명령 (스킬은 아래에서 폴더를 읽어 더한다)
BUILTIN_COMMANDS = [
    "clear", "compact", "model", "help", "review", "resume", "context", "cost", "init", "memory", "config",
    "status", "exit", "agents", "mcp", "permissions", "plan", "loop", "usage", "doctor", "login", "logout",
    "rewind", "export", "hooks", "ide", "skills", "fast", "code-review", "add-dir", "bug", "vim", "theme",
    "orchestration",
]
# 한국말로 읽은 것 -> 명령 (자주 쓰는 것만. 나머지는 소리 비슷한 것을 찾는다. config.toml [orca] slash_words 로 더한다)
SLASH_ALIASES = {
    "클리어": "clear", "컴팩트": "compact", "콤팩트": "compact", "모델": "model", "헬프": "help", "도움말": "help",
    "리뷰": "review", "코드리뷰": "code-review", "리쥼": "resume", "리줌": "resume", "레쥼": "resume",
    "컨텍스트": "context", "콘텍스트": "context", "코스트": "cost", "이닛": "init", "메모리": "memory",
    "컨피그": "config", "설정": "config", "스테이터스": "status", "상태": "status", "엑시트": "exit", "종료": "exit",
    "에이전트": "agents", "에이전츠": "agents", "퍼미션": "permissions", "권한": "permissions", "플랜": "plan",
    "루프": "loop", "유세지": "usage", "사용량": "usage", "닥터": "doctor", "리와인드": "rewind",
    "익스포트": "export", "훅스": "hooks", "스킬": "skills", "스킬스": "skills", "패스트": "fast",
    "오케스트레이션": "orchestration", "커밋": "commit",
}


def slash_commands(workdirs: list[str] = ()) -> list[str]:
    """쓸 수 있는 / 명령 이름: 들어 있는 명령 + 스킬·명령 폴더 (사용자, 플러그인, 열린 프로젝트)."""
    names = set(BUILTIN_COMMANDS)
    home = Path.home() / ".claude"
    roots = [home / "skills", home / "commands", home / "plugins" / "cache"]
    roots += [Path(d) / ".claude" / sub for d in workdirs for sub in ("skills", "commands")]
    for root in roots:
        if not root.is_dir():
            continue
        try:
            for skill in root.rglob("SKILL.md"):
                names.add(skill.parent.name)
            for command in root.rglob("*.md"):
                if command.parent.name == "commands":
                    names.add(command.stem)
        except OSError:
            continue
    return sorted(n.lower() for n in names if re.fullmatch(r"[A-Za-z0-9:_-]+", n))


def _romanize(word: str) -> str:
    """한글을 소리 나는 대로 로마자로 대충 바꾼다 (비슷한 영어 명령을 찾는 데만 쓴다)."""
    lead = ["k", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s", "ss", "", "j", "jj", "ch", "k", "t", "p", "h"]
    vowel = ["a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa", "wae", "oe", "yo", "u", "wo", "we", "wi",
             "yu", "eu", "ui", "i"]
    tail = ["", "k", "k", "k", "n", "n", "n", "t", "l", "l", "l", "l", "l", "l", "l", "l", "m", "p", "p", "t",
            "t", "ng", "t", "t", "k", "t", "p", "t"]
    out = []
    for ch in word:
        code = ord(ch) - 0xAC00
        if 0 <= code < 11172:
            out.append(lead[code // 588] + vowel[(code % 588) // 28] + tail[code % 28])
        else:
            out.append(ch.lower())
    return "".join(out)


def _sound(word: str) -> str:
    """영어·로마자를 비슷한 소리끼리 같게 만든다 (c/k/q, l/r, v/b, f/p, 모음 줄이기)."""
    word = word.lower().replace("-", "")
    for a, b in (("ph", "p"), ("ck", "k"), ("c", "k"), ("q", "k"), ("x", "ks"), ("l", "r"), ("v", "b"),
                 ("f", "p"), ("z", "j"), ("th", "s"), ("eo", "o"), ("eu", "u"), ("ae", "e"), ("rr", "r")):
        word = word.replace(a, b)
    return re.sub(r"[aeiouyw]+", "a", word)


def fix_slash(text: str, commands: list[str], aliases: dict[str, str]) -> str:
    """"슬래시 커밋" -> "/commit". 명령을 못 알아보면 "/" 뒤에 들은 말을 그대로 둔다."""
    if "/" not in text and not SLASH_WORD.search(text):
        return text
    text = SLASH_WORD.sub("/", text)
    known = {_sound(c): c for c in commands}

    def find(word: str, cutoff: float) -> str | None:
        lower = word.lower()
        if lower in commands:
            return lower
        if word in aliases:
            return aliases[word]
        close = difflib.get_close_matches(_sound(_romanize(word)), known.keys(), n=1, cutoff=cutoff)
        return known[close[0]] if close else None

    def replace(match: re.Match) -> str:
        first, second = match.group(1), match.group(2)
        # "스킬 크리에이터" -> skill-creator 처럼 두 낱말로 들린 명령을 먼저 본다
        if second:
            both = find(first + second, 0.75)
            if both:
                return "/" + both
        return "/" + (find(first, 0.6) or first) + (" " + second if second else "")

    # 받아 적을 때 "/ commit." 처럼 띄어 쓰거나 점이 붙어 나오기도 한다
    word = r"([0-9A-Za-z가-힣:_-]+)"
    text = re.sub(r"/\s*" + word + r"\.?(?:\s+" + word + r")?", replace, text)
    return text.strip()


# --- 세션 목록 ---

# ORCA 앱 설정 폴더 (Electron). 탭에 사용자가 붙인 이름은 CLI 가 알려주지 않아 여기 저장된 것을 읽는다
if sys.platform == "darwin":
    ORCA_DATA = Path.home() / "Library" / "Application Support" / "orca"
elif sys.platform == "win32":
    ORCA_DATA = Path(os.environ.get("APPDATA", Path.home())) / "orca"
else:
    ORCA_DATA = Path.home() / ".config" / "orca"


def _custom_titles() -> dict[str, str]:
    """탭 id -> 사용자가 붙인 탭 이름 (ORCA 에서 탭 이름 바꾸기). 못 읽으면 빈 dict.

    ORCA 는 탭 목록을 profiles/<프로필>/profile-state.db 의 workspaceSession 문서(JSON)에 저장한다.
    terminal list 의 title 은 Claude 가 붙인 자동 제목이라, 이 이름이 있으면 이것을 쓴다.
    """
    titles: dict[str, str] = {}
    for db in ORCA_DATA.glob("profiles/*/profile-state.db"):
        try:
            # 읽기만 한다 (ORCA 가 쓰는 중이어도 막지 않게)
            con = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True, timeout=1)
            try:
                row = con.execute(
                    "select payload from profile_state_documents where domain = 'workspaceSession'").fetchone()
            finally:
                con.close()
            session = json.loads(row[0]) if row else {}
        except (sqlite3.Error, ValueError, TypeError):
            continue
        for tabs in (session.get("tabsByWorktree") or {}).values():
            for tab in tabs or []:
                name = (tab.get("customTitle") or "").strip() if isinstance(tab, dict) else ""
                if name and tab.get("id"):
                    titles[tab["id"]] = name
    return titles


def _short_title(title: str) -> str:
    # ORCA 는 탭 이름 앞에 작업 중 표시(◐ ✳ 등)를 붙인다
    return re.sub(r"^[^\w가-힣/]+\s*", "", title or "").strip() or "터미널"


def _front_windows() -> bool:
    """Windows: ORCA 창을 앞으로 띄운다 (최소화돼 있으면 되살린다). 잘 되면 True."""
    import ctypes
    from ctypes import wintypes

    user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _):
        if not user32.IsWindowVisible(hwnd) or user32.GetWindow(hwnd, 4):  # 4: GW_OWNER (딸린 작은 창은 뺀다)
            return True
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        process = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not process:
            return True
        try:
            name = ctypes.create_unicode_buffer(1024)
            size = wintypes.DWORD(len(name))
            if kernel32.QueryFullProcessImageNameW(process, 0, name, ctypes.byref(size)):
                if os.path.basename(name.value).lower() == "orca.exe" and user32.GetWindowTextLengthW(hwnd):
                    found.append(hwnd)
                    return False
        finally:
            kernel32.CloseHandle(process)
        return True

    user32.EnumWindows(visit, 0)
    if not found:
        return False
    hwnd = found[0]
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    # 뒤에서 도는 프로그램은 창을 앞으로 못 띄우게 막혀 있다. Alt 키를 한 번 눌렀다 떼면 허용된다
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    return bool(user32.SetForegroundWindow(hwnd))


class Orca:
    def __init__(self, cfg: dict) -> None:
        self.cfg = cfg
        self.enabled = cfg.get("enabled", True)
        self.cli: str | None = None
        self.handles: dict[str, str] = {}  # 기기에 보낸 짧은 id -> orca 터미널 handle
        self.workdirs: list[str] = []
        # 기기에서 세션을 연 시각 (handle -> time.time()). 최근에 연 것을 앞에 둔다. 도우미를 다시 켜도 남긴다
        self.recent: dict[str, float] = {}
        try:
            self.recent = json.loads(RECENT_FILE.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            pass
        self.state: dict | None = None
        self.sent: dict | None = None
        self.pending_toast: str | None = None
        # 두 낱말로 적은 것("깃 푸시")은 붙여서 찾는다
        words = {k.replace(" ", ""): v.lstrip("/") for k, v in cfg.get("slash_words", {}).items()}
        self.aliases = {**SLASH_ALIASES, **words}
        self._commands: list[str] | None = None
        self._wake = threading.Event()
        if self.enabled:
            threading.Thread(target=self._poll, daemon=True).start()

    def _run(self, *args: str, timeout: float = 10) -> dict | None:
        if self.cli is None:
            self.cli = find_orca()
            if self.cli is None:
                return None
        try:
            out = subprocess.run([self.cli, *args, "--json"], capture_output=True, text=True, encoding="utf-8",
                                 timeout=timeout, stdin=subprocess.DEVNULL, creationflags=_NO_WINDOW)
        except (OSError, subprocess.TimeoutExpired) as e:
            print(f"[orca] {args[0]} {args[1]} 실패: {e}")
            self.cli = None  # ORCA 를 옮겼거나 껐을 수 있다. 다음에 다시 찾는다
            return None
        try:
            data = json.loads(out.stdout)
        except ValueError:
            return None
        return data.get("result") if data.get("ok") else None

    def _poll(self) -> None:
        while True:
            try:
                self.state = self._read()
            except Exception as e:  # 읽기 실패로 도우미가 멈추지 않게 한다
                print(f"[orca] 상태 읽기 실패: {e}")
            self._wake.wait(POLL_SECONDS)
            self._wake.clear()

    def _read(self) -> dict:
        ps = self._run("worktree", "ps")
        terms = self._run("terminal", "list")
        if ps is None or terms is None:
            return {"ok": False, "list": []}
        agents = {}
        for wt in ps.get("worktrees", []):
            for agent in wt.get("agents", []):
                agents[agent.get("paneKey")] = agent
        rows, handles = [], {}
        custom = _custom_titles()
        for term in terms.get("terminals", []):
            if term.get("orphaned") or not term.get("connected"):
                continue
            handle = term["handle"]
            short = handle.removeprefix("term_")[:8]
            agent = agents.get(f"{term.get('tabId')}:{term.get('leafId')}")
            if agent:
                # state 는 안에서 돌린 하위 작업까지 섞인 값이라, 세션 자체의 상태(mainAgent)를 먼저 본다
                state = (agent.get("mainAgent") or {}).get("state") or agent.get("state") or "idle"
            else:
                state = "working" if term.get("agentIdentity") else "shell"
            path = term.get("worktreePath") or ""
            # 사용자가 탭에 붙인 이름이 있으면 그것, 없으면 Claude 가 붙인 자동 제목
            title = custom.get(term.get("tabId")) or _short_title(term.get("title"))
            rows.append({"id": short, "t": title[:30], "p": Path(path).name[:20],
                         "s": state})
            handles[short] = handle
        # 프로젝트별로 모으고, 손댈 일이 있는 세션(승인·입력·완료)이 앞에, 작업 중과 터미널은 뒤에.
        # 같은 무리 안에서는 최근에 연 세션(과 그 프로젝트)이 앞에
        def rank(row: dict) -> tuple:
            return (row["s"] == "working", row["s"] == "shell", -self.recent.get(handles[row["id"]], 0))

        best: dict[str, tuple] = {}
        for row in rows:
            best[row["p"]] = min(best.get(row["p"], rank(row)), rank(row))
        rows.sort(key=lambda r: (best[r["p"]], r["p"], rank(r)))
        self.handles = handles
        self.workdirs = sorted({t.get("worktreePath") for t in terms.get("terminals", []) if t.get("worktreePath")})
        return {"ok": True, "list": rows[:MAX_SESSIONS]}

    def pending(self) -> dict | None:
        """기기로 보낼 것 (바뀌었을 때만). main.py 가 0.5초마다 부른다."""
        state = self.state
        if state is None or state == self.sent:
            return None
        self.sent = state
        return state

    def resend(self) -> None:
        """기기가 새로 연결됐을 때: 다음 pending() 에 다시 보낸다."""
        self.sent = None

    # --- 세션 고르기와 입력 ---

    def on_event(self, event: dict) -> None:
        """기기에서 세션 줄을 눌렀을 때: {"orca": "activate", "id": 짧은 id}. 받아쓰기({"ask": ...})는 assistant.py 가 맡는다.
        세션 팝업을 눌렀을 때: {"orca": "send", "id": 짧은 id, "key": "tab" | "enter" | "tab_enter" | 키워드 이름}."""
        if "ask" in event or not isinstance(event.get("id"), str):
            return
        if event.get("orca") == "activate":
            threading.Thread(target=self.activate, args=(event["id"],), daemon=True).start()
        elif event.get("orca") == "send" and isinstance(event.get("key"), str):
            threading.Thread(target=self.send_key, args=(event["id"], event["key"]), daemon=True).start()
        elif event.get("orca") == "reply" and isinstance(event.get("reply"), str):
            threading.Thread(target=self.reply, args=(event["id"], event["reply"]), daemon=True).start()

    def activate(self, short: str) -> str | None:
        """세션 탭으로 바꾸고 ORCA 를 앞으로 띄운다. 잘 되면 terminal handle."""
        handle = self.handles.get(short)
        if not handle:
            self.pending_toast = "ORCA 세션을 찾지 못했습니다\n목록을 새로 읽습니다"
            self._wake.set()
            return None
        if self._run("terminal", "switch", "--terminal", handle) is None:
            print(f"[orca] {short} 로 바꾸지 못했습니다")
        self.recent[handle] = time.time()
        live = set(self.handles.values())
        self.recent = {h: t for h, t in self.recent.items() if h in live}  # 닫힌 세션은 지운다
        try:
            RECENT_FILE.parent.mkdir(parents=True, exist_ok=True)
            RECENT_FILE.write_text(json.dumps(self.recent), encoding="utf-8")
        except OSError:
            pass
        self._wake.set()  # 순서가 바뀌었다
        if sys.platform == "darwin":
            subprocess.run(["open", "-b", BUNDLE_ID], capture_output=True, timeout=10)
        elif sys.platform == "win32":
            _front_windows()
        return handle

    def commands(self) -> list[str]:
        if self._commands is None:
            self._commands = slash_commands(self.workdirs)
        return self._commands

    def hotwords(self) -> str:
        """받아 적을 때 이 낱말들을 알아듣게 돕는다 (/ 명령 이름)."""
        return " ".join("/" + c for c in self.commands()[:60])

    def send_key(self, short: str, name: str) -> None:
        """세션 터미널에 키워드(글 + Enter)나 Tab / Enter 를 보낸다. 앞에 있는 창과 상관없이 그 세션에만 간다."""
        if name in KEYS:
            shown, text, enter = KEYS[name]
        elif typer.KEYWORDS.get(name):
            shown, text, enter = typer.KEYWORDS[name], typer.KEYWORDS[name], True
        else:
            print(f"[orca] 모르는 키: {name}")
            self.pending_toast = "PC 도우미가 모르는 키워드입니다\n도우미를 새로 깔아 주세요"
            return
        handle = self.handles.get(short)
        if not handle:
            self.pending_toast = "ORCA 세션을 찾지 못했습니다\n목록을 새로 읽습니다"
            self._wake.set()
            return
        ok = True
        if text:
            ok = self._run("terminal", "send", "--terminal", handle, "--text", text) is not None
            if enter and text == "\t":
                time.sleep(0.15)  # 추천 문장이 입력칸에 들어갈 틈
        if ok and enter:
            ok = self._run("terminal", "send", "--terminal", handle, "--enter") is not None
        print(f"[orca] {short} 에 {shown} {'보냈습니다' if ok else '보내지 못했습니다'}")
        self.pending_toast = f"보냈습니다: {shown}" if ok else f"ORCA 세션에 {shown}\n보내지 못했습니다"
        self._wake.set()  # 상태가 곧 바뀐다

    def reply(self, short: str, name: str) -> None:
        """빠른 답(REPLIES)을 보낸다. 상태를 새로 읽어, 지금도 그 답을 받을 상태일 때만 (늦은 목록으로 잘못 보내지 않게)."""
        if name not in REPLIES:
            print(f"[orca] 모르는 답: {name}")
            self.pending_toast = "PC 도우미가 모르는 답입니다\n도우미를 새로 깔아 주세요"
            return
        shown, text, enter, states = REPLIES[name]
        self.state = self._read()
        row = next((r for r in self.state["list"] if r["id"] == short), None)
        handle = self.handles.get(short)
        if not row or not handle:
            self.pending_toast = "ORCA 세션을 찾지 못했습니다\n목록을 새로 읽습니다"
            return
        if row["s"] not in states:
            print(f"[orca] {short} 는 지금 {row['s']} 라 {shown} 을 보내지 않았습니다")
            self.pending_toast = f"{row['t']}\n상태가 바뀌어 {shown}을 보내지 않았습니다"
            return
        ok = self._run("terminal", "send", "--terminal", handle, "--text", text) is not None
        if ok and enter:
            ok = self._run("terminal", "send", "--terminal", handle, "--enter") is not None
        print(f"[orca] {short} 에 {shown} {'보냈습니다' if ok else '보내지 못했습니다'}")
        self.pending_toast = f"{row['t']}\n{shown} 보냈습니다" if ok else f"ORCA 세션에 {shown}\n보내지 못했습니다"
        self._wake.set()  # 상태가 곧 바뀐다

    def send_text(self, handle: str, text: str) -> str | None:
        """세션 터미널에 글을 넣는다 (Enter 없이). 잘 되면 None, 안 되면 이유."""
        if self._run("terminal", "send", "--terminal", handle, "--text", text) is None:
            return "ORCA 세션에 넣지 못했습니다"
        self._wake.set()  # 상태가 곧 바뀐다
        return None
