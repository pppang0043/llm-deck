"""선택 계정과 한도 구간에 대응하는 이 PC 토큰의 API 환산. API 비용 페이지와 별개다."""

import os
import json
import time
from datetime import datetime
from pathlib import Path

from . import api_cost
from .fmt import fmt_usd

DURATIONS = {"5시간": 5 * 3600, "주간": 7 * 86400}
_scope_cache = {}


def account_files(root, state):
    active = (state.get("activeAccountIdsByRuntime") or {}).get("host", state.get("activeAccountId"))
    account = next((a for a in state.get("accounts") or [] if active and a.get("id") == active), {})
    identities = {account.get("providerAccountId"), account.get("workspaceAccountId")} - {None, ""}
    # 여러 조직 항목이 같은 provider id를 공유하면 그 id만으로 선택 조직을 특정할 수 없다.
    for other in state.get("accounts") or []:
        if other.get("id") != active:
            identities -= {other.get("providerAccountId"), other.get("workspaceAccountId")}
    if not identities:
        return []
    paths = []
    for path in root.rglob("*.jsonl"):
        try:
            stat = path.stat()
            stamp = (stat.st_mtime_ns, stat.st_size)
            cached = _scope_cache.get(path)
            if not cached or cached[0] != stamp:
                identity = None
                with path.open(encoding="utf-8", errors="replace") as stream:
                    for line in stream:
                        entry = json.loads(line)
                        if entry.get("type") == "session_meta":
                            meta = entry.get("payload") or {}
                            if meta.get("account_scoped") is True:
                                identity = meta.get("creator_account_id")
                            break
                cached = (stamp, identity)
                _scope_cache[path] = cached
            if cached[1] in identities:
                paths.append(path)
        except (OSError, ValueError, TypeError):
            continue
    return paths


def managed_home(data: dict, provider: str) -> Path | None:
    """CLI가 선택한 host 계정만 사용. 공유/기본 계정의 신원은 추정하지 않는다."""
    state = data.get(provider) or {}
    active = (state.get("activeAccountIdsByRuntime") or {}).get("host", state.get("activeAccountId"))
    account = next((a for a in state.get("accounts") or [] if a.get("id") == active and active), None)
    if not account or account.get("managedAuthRuntime", account.get("managedHomeRuntime", "host")) != "host":
        return None
    if account.get("home"):
        home = Path(account["home"])
        return home if home.is_dir() else None
    if provider != "codex":
        return None
    # Codex list는 home을 생략한다. 선택 id와 일치하는 Orca 관리 프로필만 찾는다.
    bases = []
    for other in (data.get("claude") or {}).get("accounts") or []:
        if other.get("home"):
            home = Path(other["home"])
            if home.name == "home" and home.parent.parent.name == "claude-profiles":
                bases.append(home.parent.parent.parent)
    configured = os.environ.get("CODEX_HOME")
    if configured:
        home = Path(configured)
        if home.name == "home" and home.parent.name == active and home.parent.parent.name == "codex-accounts":
            return home if home.is_dir() else None
    for base in bases:
        home = base / "codex-accounts" / active / "home"
        if home.is_dir():
            return home
    return None


def collect(pages: list, data: dict | None) -> dict | None:
    limits = [page for page in pages if page.get("bars")]
    if not limits:
        return None
    data = data or {}
    columns = []
    now = time.time()
    for page in limits:
        provenance = page.get("estimate_source") or {}
        provider = provenance.get("provider")
        home = managed_home(data, provider) if provider in ("claude", "codex") else None
        updated = provenance.get("as_of")
        root = home / ("projects" if provider == "claude" else "sessions") if home else None
        reader = api_cost._read_claude if provider == "claude" else api_cost._read_codex
        # 프로필 logs가 공용 폴더 symlink일 수 있어 경로만으로 계정을 판별하지 않는다.
        # Claude 로그에는 대응하는 계정 provenance가 없어 % 환산에 사용하지 않는다.
        records = None
        if provider == "codex" and root and root.is_dir():
            paths = account_files(root, data.get("codex") or {})
            if paths:
                records = api_cost._records(root, reader, paths=paths)
        values = {}
        for bar in page["bars"]:
            label, reset = bar.get("label"), bar.get("resets")
            duration = DURATIONS.get(label)
            values[label] = "환산불가"
            if (records is None or not duration or not isinstance(reset, (int, float))
                    or not isinstance(updated, (int, float)) or reset <= now
                    or updated > now + 60 or now - updated > 600):
                continue
            start, end = reset - duration, min(updated, now)
            if not start <= end < reset:
                continue
            matching = []
            for timestamp, usd, _ in records.values():
                try:
                    ts = datetime.fromisoformat(timestamp).timestamp()
                except ValueError:
                    continue
                if start <= ts <= end:
                    matching.append(usd)
            if not matching:
                values[label] = "기록없음"
            elif all(usd is not None for usd in matching):
                values[label] = fmt_usd(sum(matching))
        columns.append((provenance.get("title") or page["name"], values))
    labels = list(dict.fromkeys(bar["label"] for page in limits for bar in page["bars"]))
    return {"name": "Usage Value", "title": "Token Value", "note": "USD",
            "side": True, "usage_value": True,
            "table": [[""] + [title for title, _ in columns]] +
                     [[label] + [values.get(label, "환산불가") for _, values in columns] for label in labels],
            "sub": "API 환산 · 청구 아님\n같은 구간의 이 PC 기록"}
