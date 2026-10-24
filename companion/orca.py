"""ORCA 에 열린 세션들의 작업 상태를 기기로 보내고, 기기에서 고른 세션에 받아쓰기한 글을 입력한다.

  - 10초마다 orca CLI 로 세션 목록과 상태를 읽는다 (orca worktree ps, orca terminal list). 바뀌었을 때만 기기로 보낸다.
      PC -> 기기: {"orca": {"ok": bool, "list": [{"id", "t": 이름, "p": 프로젝트, "s": 상태}]}}
      s: working(작업 중) / waiting(입력 기다림) / permission(권한 요청) / blocked / done / idle / shell(AI 없는 터미널)
  - 기기에서 세션을 누르면 {"ask": "start", "mode": "dictate", "orca": id}: 그 세션 탭으로 바꾸고 ORCA 를 앞으로 띄운다.
    받아 적은 글은 커서 자리가 아니라 그 세션 터미널에 직접 넣는다 (orca terminal send). Enter 는 누르지 않는다.
  - 기기에서 세션을 길게 누르면 빠른 답: {"orca": "reply", "id": id, "reply": "allow" | "deny" | "continue"}.
    정해 둔 답(REPLIES)만 보내고, 보내기 직전에 상태를 다시 읽어 그 답이 맞는 상태일 때만 보낸다.
  - 빠른 답 창의 숨기기: {"orca": "hide", "id": id}. 그 터미널을 목록에서 뺀다 (HIDDEN_FILE 에 적어 도우미를 다시 켜도 빠진다).
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
# ORCA 는 탭을 바꾸면 이만큼 뒤에 화면 상태를 저장한다 (재 보니 1~1.5초)
ACTIVE_TAB_LAG_SECONDS = 2.0
MAX_SESSIONS = 24  # 기기 orcapanel.cpp 의 MAX_ROWS
_NO_WINDOW = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
BUNDLE_ID = "com.stablyai.orca"  # Mac 의 ORCA 앱
HIDDEN_FILE = Path.home() / ".llm-deck" / "orca_hidden.json"  # 기기에서 숨긴 터미널 handle 들
# /orchestration 이 워커에게 주는 첫 글. 일을 끝낸 워커는 ORCA 가 parentPaneKey 를 지워서 이것으로도 알아본다
WORKER_PROMPT = "You are working inside Orca, a multi-agent IDE."
# ORCA 화면 세션 팝업의 키 (이름 -> 알림에 쓰는 이름, 보낼 글, 끝에 Enter). 나머지 이름은 typer.KEYWORDS 의 글 + Enter
# (글이 띄어쓰기로 끝나면 Enter 없이, typer.wants_enter).
# Tab + Enter 는 Claude Code 가 흐리게 보여준 추천 문장을 받아서 바로 보낸다.
# 지우기는 여러 줄도 모두 지운다 (erase_keys). Ctrl+E (줄 끝으로) + Ctrl+U (줄 처음까지 지우기) 로 한 줄을 비우고,
# Delete 로 아래 줄을, Backspace 로 위 줄을 끌어와 다시 비운다. 입력칸이 비어 있어도, Claude 가 일하는 중이어도 아무 일 없다.
# Esc 두 번 / Ctrl+C 도 입력칸을 비우지만, 빈 칸이면 되감기(/rewind) 창을 열거나 일하는 Claude 를 멈추므로 쓰지 않는다.
KEYS = {"tab": ("Tab", "\t", False), "enter": ("Enter", "", True), "tab_enter": ("Tab + Enter", "\t", True),
        # 이름이 "clear" 면 /clear 키워드(typer.KEYWORDS)와 겹쳐 /clear 를 눌러도 지우기가 됐다
        "erase": ("지우기", None, False),
        # Esc 한 번: 일하는 Claude 를 멈추거나 열린 고르기 창을 닫는다 (사용자가 Esc 버튼을 직접 누를 때만)
        "esc": ("Esc", "\x1b", False)}
# 팝업 /스킬 의 스킬 ("skill:" + 이름). "/이름 " 만 넣고 Enter 는 치지 않는다 (뒤에 할 일을 이어 적는다).
# 여기 있는 것만 입력한다. 기기 orcapanel.cpp 의 SKILLS 와 같게
SKILLS = ("proto-design", "minipc")
# Claude Code 는 키를 한 번에 많이 받으면 일부만 처리한다 (10줄 치를 몰아 보내면 글자가 섞였다). 5줄씩 나눠 보낸다
ERASE_CHUNK = 5
ERASE_BELOW = 20  # 커서 아래로 지우는 줄 수 (보통 커서는 맨 끝이라 0)
ERASE_ABOVE = 50  # 커서 위로 지우는 줄 수


def erase_keys() -> list[str]:
    """입력칸을 모두 비우는 키를 나눠 보낼 묶음들"""
    clear_line, below, above = "\x05\x15", "\x1b[3~", "\x7f"
    return ([(clear_line + below) * ERASE_CHUNK] * (ERASE_BELOW // ERASE_CHUNK)
            + [(clear_line + above) * ERASE_CHUNK] * (ERASE_ABOVE // ERASE_CHUNK) + [clear_line])


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
    r"%ProgramFiles%\Orca\resources\bin\orca.exe",
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
    # ORCA 안에서 켠 도우미는 ORCA 가 알려 준 자리가 가장 정확하다
    bin_dir = os.environ.get("ORCA_CLI_BIN_DIR")
    if bin_dir:
        for name in ("orca.exe", "orca"):
            path = os.path.join(bin_dir, name)
            if os.path.isfile(path):
                return path
    path = shutil.which("orca")
    # 끊어진 링크는 which 가 건너뛴다. Windows 에서 orca.exe 가 옆에 없는 .cmd(install.py 가 까는 orca.cmd)는
    # cmd.exe 를 거쳐 글이 망가질 수 있어 (_native) 진짜 orca.exe 를 못 찾을 때만 쓴다
    if path and not _native(path).lower().endswith((".cmd", ".bat")):
        return _native(path)
    shim = path
    for candidate in EXTRA_PATHS:
        path = os.path.expanduser(os.path.expandvars(candidate))
        if os.path.exists(path):
            return path
    if sys.platform == "darwin":
        # /Applications 밖으로 옮긴 ORCA. 임시 경로(AppTranslocation)는 재부팅하면 사라져 뺀다
        try:
            out = subprocess.run(["mdfind", f"kMDItemCFBundleIdentifier == '{BUNDLE_ID}'"],
                                 capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.TimeoutExpired):
            out = ""
        for app in out.splitlines():
            path = os.path.join(app, "Contents", "Resources", "bin", "orca")
            if "/AppTranslocation/" not in app and os.path.exists(path):
                return path
        # 다운로드한 자리에서 바로 연 ORCA 는 macOS 가 임시 경로로 옮겨 실행한다. 지금 떠 있는 ORCA 에서 찾는다
        try:
            out = subprocess.run(["ps", "-axo", "comm="], capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.TimeoutExpired):
            out = ""
        for line in out.splitlines():
            if line.endswith("/Orca.app/Contents/MacOS/Orca"):
                path = line[: -len("MacOS/Orca")] + "Resources/bin/orca"
                if os.path.exists(path):
                    return path
    return shim


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


def _profile_docs(*domains: str) -> list[dict]:
    """ORCA 가 화면 상태를 저장한 문서들 (JSON). 프로필마다 {domain: 문서}.

    ORCA 는 profiles/<프로필>/profile-state.db 에 저장한다. workspaceSession 은 탭 목록과 지금 보이는 탭,
    projects 는 사이드바의 프로젝트 (그 순서대로). 최근에 쓴 프로필(지금 쓰는 것)이 앞에.
    """
    found = []
    for db in ORCA_DATA.glob("profiles/*/profile-state.db"):
        try:
            # 읽기만 한다 (ORCA 가 쓰는 중이어도 막지 않게)
            con = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True, timeout=1)
            try:
                docs = {domain: json.loads(row[0]) for domain in domains
                        for row in [con.execute("select payload from profile_state_documents where domain = ?",
                                                (domain,)).fetchone()] if row}
            finally:
                con.close()
            wal = db.with_name(db.name + "-wal")
            changed = max(db.stat().st_mtime, wal.stat().st_mtime if wal.exists() else 0)
        except (OSError, sqlite3.Error, ValueError, TypeError):
            continue
        found.append((changed, docs))
    return [docs for _, docs in sorted(found, key=lambda f: f[0], reverse=True)]


def _workspace_sessions() -> list[dict]:
    return [d["workspaceSession"] for d in _profile_docs("workspaceSession")
            if isinstance(d.get("workspaceSession"), dict)]


def _screen_order() -> tuple[dict[str, int], dict[str, int]]:
    """ORCA 화면에 보이는 순서: (저장소 id -> 사이드바의 프로젝트 순서, 탭 id -> 그 작업 폴더 안의 탭 순서).
    못 읽으면 빈 dict (그때는 orca terminal list 순서대로)."""
    docs = _profile_docs("workspaceSession", "projects")
    if not docs:
        return {}, {}
    repos: dict[str, int] = {}
    projects = docs[0].get("projects")
    for i, project in enumerate(projects if isinstance(projects, list) else []):
        for repo in (project.get("sourceRepoIds") or []) if isinstance(project, dict) else []:
            repos.setdefault(repo, i)
    tabs: dict[str, int] = {}
    session = docs[0].get("workspaceSession")
    groups_by_worktree = session.get("tabGroups") if isinstance(session, dict) else None
    for groups in (groups_by_worktree or {}).values():
        n = 0
        for group in groups or []:
            for tab in (group.get("tabOrder") or []) if isinstance(group, dict) else []:
                tabs.setdefault(tab, n)
                n += 1
    return repos, tabs


def active_tab() -> str | None:
    """ORCA 화면에 지금 보이는 탭 id. 못 읽으면 None.
    탭을 바꾸면 ORCA 가 1~1.5초 뒤에 저장하므로 그만큼 늦다 (orca terminal show 는 화면을 따라가지 않는다)."""
    sessions = _workspace_sessions()
    tab = sessions[0].get("activeTabId") if sessions else None
    return tab if isinstance(tab, str) else None


def _custom_titles() -> dict[str, str]:
    """탭 id -> 사용자가 붙인 탭 이름 (ORCA 에서 탭 이름 바꾸기). 못 읽으면 빈 dict.
    terminal list 의 title 은 Claude 가 붙인 자동 제목이라, 이 이름이 있으면 이것을 쓴다.
    """
    titles: dict[str, str] = {}
    for session in _workspace_sessions():
        for tabs in (session.get("tabsByWorktree") or {}).values():
            for tab in tabs or []:
                name = (tab.get("customTitle") or "").strip() if isinstance(tab, dict) else ""
                if name and tab.get("id"):
                    titles[tab["id"]] = name
    return titles


def _short_title(title: str) -> str:
    # ORCA 는 탭 이름 앞에 작업 중 표시(◐ ✳ 등)를 붙인다
    return re.sub(r"^[^\w가-힣/]+\s*", "", title or "").strip() or "터미널"


def _copy(text: str) -> bool:
    """클립보드에 넣는다 (넣지 못한 받아쓰기를 잃지 않게). 잘 되면 True."""
    try:
        if sys.platform == "darwin":
            subprocess.run(["pbcopy"], input=text.encode("utf-8"), timeout=5, check=True)
        elif sys.platform == "win32":
            subprocess.run(["clip"], input=text.encode("utf-16"), timeout=5, check=True, creationflags=_NO_WINDOW)
        else:
            return False
        return True
    except (OSError, subprocess.SubprocessError):
        return False


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
        self.tabs: dict[str, str] = {}  # 터미널 handle -> ORCA 탭 id (화면에 보이는지 볼 때)
        self.workdirs: list[str] = []
        self.state: dict | None = None
        self.sent: dict | None = None
        self.pending_toast: str | None = None
        try:
            self.hidden: set[str] = set(json.loads(HIDDEN_FILE.read_text(encoding="utf-8")))
        except (OSError, ValueError, TypeError):
            self.hidden = set()
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
        rows, handles, tabs = [], {}, {}
        order_keys = []  # (작업 폴더 id, 탭 id, 짧은 id). terminal list 순서
        custom = _custom_titles()
        for term in terms.get("terminals", []):
            if term.get("orphaned") or not term.get("connected"):
                continue
            handle = term["handle"]
            short = handle.removeprefix("term_")[:8]
            agent = agents.get(f"{term.get('tabId')}:{term.get('leafId')}")
            if agent and (agent.get("parentPaneKey") or (agent.get("prompt") or "").startswith(WORKER_PROMPT)):
                continue  # /orchestration 이 띄운 워커는 그 세션 안의 일이라 목록에 넣지 않는다
            if handle in self.hidden:
                continue
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
            tabs[handle] = term.get("tabId")
            order_keys.append((term.get("worktreeId") or path, term.get("tabId"), short))
        # ORCA 화면에 떠 있는 순서대로: 사이드바의 프로젝트 순서, 그 안에서 탭 순서.
        # (상태로 정렬하면 상태가 바뀔 때마다 버튼 자리가 바뀌어 헷갈린다) 모르는 것은 terminal list 순서대로 뒤에
        repo_order, tab_order = _screen_order()
        wt_first: dict[str, int] = {}
        for i, key in enumerate(order_keys):
            wt_first.setdefault(key[0], i)
        rank = {short: (repo_order.get(wt.split("::")[0], len(repo_order)), wt_first[wt],
                        tab_order.get(tab, len(tab_order)), i)
                for i, (wt, tab, short) in enumerate(order_keys)}
        rows.sort(key=lambda r: rank[r["id"]])
        live = {t["handle"] for t in terms.get("terminals", [])}
        if self.hidden - live:  # 닫힌 터미널은 숨긴 목록에서도 지운다
            self.hidden &= live
            self._save_hidden()
        self.handles = handles
        self.tabs = tabs
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
        세션 팝업을 눌렀을 때: {"orca": "send", "id": 짧은 id, "key": "tab" | "enter" | "tab_enter" | "skill:" 스킬 | 키워드 이름}."""
        if "ask" in event or not isinstance(event.get("id"), str):
            return
        if event.get("orca") == "activate":
            threading.Thread(target=self.activate, args=(event["id"],), daemon=True).start()
        elif event.get("orca") == "send" and isinstance(event.get("key"), str):
            threading.Thread(target=self.send_key, args=(event["id"], event["key"]), daemon=True).start()
        elif event.get("orca") == "reply" and isinstance(event.get("reply"), str):
            threading.Thread(target=self.reply, args=(event["id"], event["reply"]), daemon=True).start()
        elif event.get("orca") == "hide":
            self.hide(event["id"])

    def hide(self, short: str) -> None:
        """기기에서 숨긴 세션을 목록에서 뺀다. 그 터미널을 닫을 때까지."""
        handle = self.handles.get(short)
        if not handle:
            self.pending_toast = "ORCA 세션을 찾지 못했습니다\n목록을 새로 읽습니다"
            self._wake.set()
            return
        self.hidden.add(handle)
        self._save_hidden()
        print(f"[orca] {short} 를 숨겼습니다")
        self._wake.set()

    def _save_hidden(self) -> None:
        try:
            HIDDEN_FILE.parent.mkdir(parents=True, exist_ok=True)
            HIDDEN_FILE.write_text(json.dumps(sorted(self.hidden)), encoding="utf-8")
        except OSError as e:
            print(f"[orca] 숨긴 목록을 적지 못했습니다: {e}")

    def activate(self, short: str) -> str | None:
        """세션 탭으로 바꾸고 ORCA 를 앞으로 띄운다. 잘 되면 terminal handle."""
        handle = self.handles.get(short)
        if not handle:
            self.pending_toast = "ORCA 세션을 찾지 못했습니다\n목록을 새로 읽습니다"
            self._wake.set()
            return None
        if self._run("terminal", "switch", "--terminal", handle) is None:
            print(f"[orca] {short} 로 바꾸지 못했습니다")
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
        elif name.startswith("skill:") and name[6:] in SKILLS:
            shown, text, enter = "/" + name[6:], f"/{name[6:]} ", False
        elif typer.KEYWORDS.get(name):
            text = typer.KEYWORDS[name]
            shown, enter = text.strip(), typer.wants_enter(text)
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
        if text is None:
            for keys in erase_keys():
                ok = ok and self._run("terminal", "send", "--terminal", handle, "--text", keys) is not None
        elif text:
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

    def _tab_title(self, tab: str | None) -> str:
        handle = next((h for h, t in self.tabs.items() if t == tab), None)
        return self._title(handle) if handle else "다른 탭"

    def _title(self, handle: str) -> str:
        short = handle.removeprefix("term_")[:8]
        row = next((r for r in (self.state or {}).get("list", []) if r["id"] == short), None)
        return row["t"] if row else short

    def showing(self, handle: str) -> tuple[bool, str | None]:
        """ORCA 화면에 지금 그 세션이 보이는지, 그리고 보이는 탭. 화면 상태를 못 읽으면 (예전 ORCA 등) 보이는 것으로 본다."""
        want = self.tabs.get(handle)
        active = active_tab()
        if not want or not active or active == want:
            return True, active
        # 막 바꾼 탭은 ORCA 가 늦게 저장하므로 한 번 더 기다려 본다 (짧게 말하면 끝날 때까지 아직 안 바뀌어 있다)
        time.sleep(ACTIVE_TAB_LAG_SECONDS)
        active = active_tab()
        return active == want, active

    def send_text(self, handle: str, text: str) -> str | None:
        """세션 터미널에 글을 넣는다 (Enter 없이). 잘 되면 None, 안 되면 이유.
        넣기 직전에 ORCA 화면에 그 세션이 보이는지 확인한다. 받아 적는 동안 다른 세션(에이전트)이 자기 세션을
        앞으로 띄우면 글이 그쪽으로 넘어간 적이 있어, 다르면 넣지 않고 글을 클립보드에 남긴다."""
        ok, active = self.showing(handle)
        if not ok:
            now = self._tab_title(active)
            print(f"[orca] 넣지 않음: 고른 세션 {self._title(handle)}, 지금 보이는 세션 {now}")
            copied = _copy(text)
            return (f"ORCA 화면이 다른 세션({now})으로 바뀌어 넣지 않았습니다"
                    + ("\n받아 적은 글은 클립보드에 있습니다" if copied else ""))
        if self._run("terminal", "send", "--terminal", handle, "--text", text) is None:
            return "ORCA 세션에 넣지 못했습니다"
        print(f"[orca] {self._title(handle)} 에 받아 적은 글을 넣었습니다")
        self._wake.set()  # 상태가 곧 바뀐다
        return None
