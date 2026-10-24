"""구독 한도 사용률을 서버에 직접 묻는다 (Claude Code 의 /usage, Codex 의 /status 가 쓰는 곳).

상태줄 파일이나 세션 기록은 이 PC 에서 마지막으로 응답을 받은 순간의 값이라, 다른 기기(웹, 폰, 다른 PC)에서
쓴 양이 늦게 반영된다. 서버 값은 계정 전체 기준이라 바로 맞다. 이 PC 에 로그인된 정보를 그대로 쓰고,
로그인이 만료됐으면 묻지 않는다 (새로 받는 것은 Claude Code / Codex 가 쓸 때 스스로 한다. 여기서 받으면
그쪽 로그인이 풀릴 수 있다). 공개 문서가 없는 주소라 실패하면 None 을 돌려주고, 수집기는 기존 값을 쓴다.
토큰은 각 회사 서버로만 보내고 어디에도 남기지 않는다.
"""

import base64
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

INTERVAL = 120  # 이만큼마다 묻는다
RETRY_AFTER_FAIL = 600  # 실패하면 이만큼 쉰다
MAX_BACKOFF = 3600  # 너무 자주 묻는다(429)고 하면 쉬는 시간을 두 배씩 늘리되 이만큼까지만
# 도우미를 다시 켜도 곧바로 묻지 않게 다음에 물을 시각을 남겨 둔다 (재시작 직후에 429 가 났다)
STATE = Path.home() / ".llm-deck" / "server-usage.json"

_cache: dict[str, tuple[float, dict | None]] = {}  # 이름: (다음에 물을 시각, 마지막 값)
_backoff: dict[str, float] = {}  # 429 뒤 지금 쉬는 시간
_quiet: set[str] = set()  # 실패를 이미 알린 이름. 성공할 때까지 다시 알리지 않는다


def _get(url: str, headers: dict) -> dict:
    req = urllib.request.Request(url, headers={**headers, "User-Agent": "llm-deck"})
    with urllib.request.urlopen(req, timeout=10) as res:
        return json.load(res)


def _load_next(name: str) -> float:
    try:
        return float(json.loads(STATE.read_text(encoding="utf-8"))[name])
    except (OSError, ValueError, KeyError, TypeError):
        return 0.0


def _save_next(name: str, at: float) -> None:
    try:
        state = json.loads(STATE.read_text(encoding="utf-8"))
        if not isinstance(state, dict):
            state = {}
    except (OSError, ValueError):
        state = {}
    state[name] = at
    try:
        STATE.parent.mkdir(parents=True, exist_ok=True)
        STATE.write_text(json.dumps(state), encoding="utf-8")
    except OSError:
        pass


def _retry_after(e: urllib.error.HTTPError) -> float:
    try:
        return float(e.headers.get("Retry-After") or 0)
    except (TypeError, ValueError):
        return 0.0


def _cached(name: str, fetch) -> dict | None:
    now = time.time()
    if name not in _cache:
        _cache[name] = (_load_next(name), None)
    next_at, value = _cache[name]
    if now < next_at:
        return value
    try:
        value = fetch()
        _backoff.pop(name, None)
        _quiet.discard(name)
        next_at = now + (INTERVAL if value else RETRY_AFTER_FAIL)
    except urllib.error.HTTPError as e:
        if e.code != 429:
            return _failed(name, now, e)
        wait = min(MAX_BACKOFF, max(_backoff.get(name, RETRY_AFTER_FAIL / 2) * 2, _retry_after(e)))
        if name not in _quiet:
            _quiet.add(name)
            print(f"[server] {name} 사용량 서버가 너무 자주 묻는다고 해서 {wait / 60:.0f}분 쉽니다 (기존 값 사용)")
        _backoff[name] = wait
        next_at = now + wait  # 그동안은 마지막으로 읽은 값을 그대로 쓴다
    except Exception as e:  # 주소가 바뀌었거나 네트워크 문제. 기존 값으로 넘어간다
        return _failed(name, now, e)
    _cache[name] = (next_at, value)
    _save_next(name, next_at)
    return value


def _failed(name: str, now: float, e: Exception) -> None:
    if name not in _quiet:
        _quiet.add(name)
        print(f"[server] {name} 사용량을 서버에서 읽지 못했습니다: {type(e).__name__} {e}")
    _cache[name] = (now + RETRY_AFTER_FAIL, None)
    _save_next(name, now + RETRY_AFTER_FAIL)
    return None


def _minute(t: float) -> int:
    # 서버의 초기화 시각은 물을 때마다 1초 안쪽으로 흔들린다. 분으로 맞춰 같은 값이 되게 한다
    return int(round(t / 60) * 60)


# ---- Claude ----

def _claude_token() -> str | None:
    raw = ""
    if sys.platform == "darwin":
        try:
            raw = subprocess.run(["security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
                                 capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            raw = ""
    if not raw:
        path = Path(os.environ.get("CLAUDE_CONFIG_DIR", Path.home() / ".claude")) / ".credentials.json"
        try:
            raw = path.read_text(encoding="utf-8")
        except OSError:
            return None
    try:
        cred = json.loads(raw)["claudeAiOauth"]
    except (ValueError, KeyError, TypeError):
        return None
    if cred.get("expiresAt", 0) / 1000 < time.time() + 60:
        return None  # 만료. Claude Code 를 쓰면 새로 받아 둔다
    return cred.get("accessToken")


def _fetch_claude() -> dict | None:
    token = _claude_token()
    if not token:
        return None
    data = _get("https://api.anthropic.com/api/oauth/usage",
                {"Authorization": f"Bearer {token}", "anthropic-beta": "oauth-2025-04-20"})
    windows = []
    for key, label in (("five_hour", "5h"), ("seven_day", "Week")):
        w = data.get(key)
        if not isinstance(w, dict) or w.get("utilization") is None:
            continue
        resets = w.get("resets_at")
        resets_at = _minute(datetime.fromisoformat(resets).timestamp()) if resets else None
        windows.append([label, float(w["utilization"]), resets_at])
    return {"name": "Claude Code", "as_of": time.time(), "windows": windows} if windows else None


def claude() -> dict | None:
    """{"name", "as_of", "windows": [[이름, 사용률 %, 초기화 시각], ...]}. 못 읽으면 None."""
    return _cached("claude", _fetch_claude)


# ---- Codex ----

def _jwt_exp(token: str) -> float:
    try:
        part = token.split(".")[1]
        return json.loads(base64.urlsafe_b64decode(part + "=" * (-len(part) % 4))).get("exp", 0)
    except (IndexError, ValueError):
        return 0


def _fetch_codex() -> dict | None:
    path = Path(os.environ.get("CODEX_HOME", Path.home() / ".codex")) / "auth.json"
    try:
        tokens = json.loads(path.read_text(encoding="utf-8"))["tokens"]
        token = tokens["access_token"]
    except (OSError, ValueError, KeyError, TypeError):
        return None
    if _jwt_exp(token) < time.time() + 60:
        return None  # 만료. Codex 를 쓰면 새로 받아 둔다
    data = _get("https://chatgpt.com/backend-api/wham/usage",
                {"Authorization": f"Bearer {token}", "ChatGPT-Account-Id": tokens.get("account_id") or ""})
    limits = data.get("rate_limit") or {}
    windows = []
    for key in ("primary_window", "secondary_window"):
        w = limits.get(key)
        if not isinstance(w, dict):
            continue
        seconds = w.get("limit_window_seconds") or 0
        label = {18000: "5h", 604800: "Week"}.get(seconds, f"{seconds // 3600}h")
        resets_at = w.get("reset_at")
        windows.append([label, float(w.get("used_percent") or 0), _minute(resets_at) if resets_at else None])
    return {"name": "Codex", "as_of": time.time(), "windows": windows} if windows else None


def codex() -> dict | None:
    """{"name", "as_of", "windows": [[이름, 사용률 %, 초기화 시각], ...]}. 못 읽으면 None."""
    return _cached("codex", _fetch_codex)
