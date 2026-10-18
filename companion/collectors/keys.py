"""청구액 Admin 키 상태: 만료가 다가올 때와 서버가 키를 거절할 때 화면에 보여줄 문구 (앞에 회사 이름이 붙는다).

키의 만료일은 서버에 물어볼 방법이 마땅치 않아서, 키를 만들 때 고른 날짜를 config.toml 의 expires 에 적어 둔다.
한 번이라도 통했던 키가 401 을 받으면 만료(또는 삭제)로 본다. 통했던 키는 지문(해시 앞부분)만 남긴다.
"""

import hashlib
import json
from datetime import date

from statusline import DATA_DIR

STATE = DATA_DIR / "admin-keys.json"
# 만료 며칠 전부터 알릴지
WARN_DAYS = 7


def _fingerprint(key: str) -> str:
    return hashlib.sha256(key.encode()).hexdigest()[:16]


def _load() -> dict:
    try:
        return json.loads(STATE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def mark_ok(section: str, key: str) -> None:
    """이 키가 통했다고 적어 둔다 (나중에 401 이 오면 만료로 판단하려고)."""
    state = _load()
    if state.get(section) == _fingerprint(key):
        return
    state[section] = _fingerprint(key)
    try:
        DATA_DIR.mkdir(exist_ok=True)
        STATE.write_text(json.dumps(state), encoding="utf-8")
    except OSError:
        pass


def expires(cfg: dict) -> date | None:
    try:
        return date.fromisoformat(cfg.get("expires", "").strip())
    except ValueError:
        return None


def expiry_note(cfg: dict) -> str:
    """키가 통할 때 보여줄 만료 안내. 아직 멀었으면 빈 글자."""
    until = expires(cfg)
    if not until:
        return ""
    left = (until - date.today()).days
    if left < 0:
        return f"키 만료일({until})이 지났습니다. config.toml 의 expires 를 확인하세요"
    if left == 0:
        return "오늘 키가 만료됩니다. 새 키를 만들어 두세요"
    if left <= WARN_DAYS:
        return f"키 만료 {left}일 전 ({until}). 새 키를 만들어 두세요"
    return ""


def rejected_text(section: str, key: str, cfg: dict, code: int, create_at: str) -> str:
    """서버가 키를 거절(401/403)했을 때 보여줄 안내."""
    until = expires(cfg)
    expired = until is not None and until <= date.today()
    worked_before = _load().get(section) == _fingerprint(key)
    if code == 401 and (expired or worked_before):
        when = f" ({until})" if until else ""
        sub = f"키가 만료됐거나 삭제됐습니다{when}. {create_at} 에서 새 키를 만들어 config.toml 에 넣으세요."
    elif code == 401:
        sub = f"키가 맞지 않습니다. {create_at} 에서 만든 Admin 키를 따옴표 안에 빠짐없이 넣었는지 확인하세요."
    else:
        sub = f"이 키로는 청구액을 볼 권한이 없습니다. 조직 관리자가 {create_at} 에서 만든 Admin 키가 필요합니다."
    return f"{sub} (HTTP {code})"
