"""ORCA 가 아는 Claude, Codex 계정의 구독 한도 (`orca account list --json`).

- 본 페이지(Claude, Codex)는 이 PC 의 ORCA 가 지금 고른 계정 값이다. 계정을 바꾸면 이름도 바뀌어
  기기의 그래프, 소진 예상, 80% 알림이 계정마다 따로 쌓인다.
- 계정 페이지(Claude 계정, Codex 계정)는 ORCA 에 등록된 계정을 모두 한 줄씩 보인다. 고르지 않은 계정은
  ORCA 가 마지막으로 읽은 값이라 오래됐을 수 있어 시각을 함께 적는다. 막대가 아니라 줄이라 알림과 기록은 없다.

토큰은 읽지 않는다. ORCA 가 꺼져 있거나 CLI 가 없으면 None 이고, 그때는 기존 수집기 값을 그대로 쓴다.
"""

import json
import math
import subprocess
import time

import orca
from collectors.fmt import limits_page

CACHE_S = 15  # 사용량은 몇 초마다 모은다. ORCA 는 이보다 자주 묻지 않는다
STALE_S = 600  # 이보다 오래된 값은 줄에 언제 값인지 적는다

PROVIDERS = (("claude", "claude_code", "Claude"), ("codex", "codex", "Codex"))

_cache: tuple[float, dict | None] = (0.0, None)


def read() -> dict | None:
    global _cache
    if time.monotonic() - _cache[0] < CACHE_S:
        return _cache[1]
    data = None
    command = orca.find_orca()
    if command:
        try:
            result = subprocess.run([command, "account", "list", "--json"], capture_output=True, text=True,
                                    encoding="utf-8", timeout=10, creationflags=orca._NO_WINDOW)
            envelope = json.loads(result.stdout) if not result.returncode else {}
            got = envelope.get("result")
            if envelope.get("ok") and isinstance(got, dict) and isinstance(got.get("rateLimits"), dict):
                data = got
        except (OSError, subprocess.SubprocessError, ValueError, TypeError, AttributeError):
            pass
    _cache = (time.monotonic(), data)
    return data


def _org(account: dict) -> str:
    org = account.get("organizationName") or account.get("workspaceLabel") or ""
    return "개인" if org.endswith("'s Organization") else org


def _accounts(state: dict) -> tuple[str | None, list[dict]]:
    """(지금 고른 계정 id, [{"id", "email", "org"}]). 고른 계정이 없으면 PC 기본 로그인을 id None 으로 넣는다."""
    active = (state.get("activeAccountIdsByRuntime") or {}).get("host", state.get("activeAccountId"))
    accounts = [{"id": a.get("id"), "email": a.get("email") or "", "org": _org(a)}
                for a in state.get("accounts") or [] if a.get("managedAuthRuntime", a.get("managedHomeRuntime", "host")) == "host"]
    if not active:
        default = state.get("systemDefault") or {}
        email = default.get("email") or state.get("systemDefaultEmail") or ""
        accounts.insert(0, {"id": None, "email": email, "org": _org(default)})
    return active, accounts


def _labels(accounts: list[dict]) -> dict:
    """id -> 화면 이름. 메일 앞부분, 같은 메일이 둘이면 조직 이름을 붙인다."""
    labels = {}
    for a in accounts:
        name = a["email"].split("@")[0] or "PC 기본"
        if a["org"] and sum(b["email"] == a["email"] for b in accounts) > 1:
            name = f"{name} {a['org']}"
        labels[a["id"]] = name[:20]
    return labels


def _windows(usage: dict) -> list:
    windows = []
    for key, label in (("session", "5h"), ("weekly", "Week")):
        w = usage.get(key)
        pct = w.get("usedPercent") if isinstance(w, dict) else None
        if not isinstance(pct, (int, float)) or not math.isfinite(pct):
            continue
        resets = w.get("resetsAt")
        windows.append([label, pct, resets / 1000 if isinstance(resets, (int, float)) and resets else None])
    return windows


def _cell(windows: list, updated: float | None, now: float) -> str:
    if not windows:
        return "값 없음"
    by = {label: (0 if resets and resets <= now else round(pct)) for label, pct, resets in windows}
    text = " / ".join(f"{by[k]}%" if k in by else "-" for k in ("5h", "Week"))
    age = now - updated if updated else None
    if age is not None and age > STALE_S:
        text += f" · {int(age // 86400)}일 전" if age >= 86400 else \
            f" · {int(age // 3600)}시간 전" if age >= 3600 else f" · {int(age // 60)}분 전"
    return text


def pages(data: dict, cfg: dict) -> tuple[dict, list]:
    """({설정 섹션: 본 페이지}, [계정 페이지, ...]). 대상이 이 PC(host)가 아닌 서비스는 뺀다."""
    main, extra = {}, []
    now = time.time()
    limits = data.get("rateLimits") or {}
    for provider, section, title in PROVIDERS:
        if not cfg.get(section, {}).get("enabled", True):
            continue
        if (limits.get(provider + "Target") or {}).get("runtime", "host") != "host":
            continue
        active, accounts = _accounts(data.get(provider) or {})
        labels = _labels(accounts)
        usage = limits.get(provider) or {}
        windows, updated = _windows(usage), usage.get("updatedAt")
        name = f"{title} {labels.get(active, 'PC 기본')}"
        if windows and isinstance(updated, (int, float)) and updated > 0:
            page = limits_page(name, {"as_of": updated / 1000, "windows": windows})
            page["estimate_source"] = {"provider": provider, "as_of": updated / 1000, "title": title}
            if usage.get("error"):
                page["sub"] = "ORCA 갱신 실패: 마지막 값"
        else:
            page = {"name": name, "headline": "-", "sub": "ORCA 에서 사용량을 아직 못 읽었습니다."}
        main[section] = page

        others = {i.get("accountId"): i for i in limits.get("inactive" + title + "Accounts") or []}
        rows = [["계정", "5시간 / 주간"]]
        for a in accounts:
            if a["id"] == active:
                cell = _cell(windows, updated / 1000 if updated else None, now)
            else:
                other = others.get(a["id"]) or {}
                got = other.get("rateLimits") or {}
                at = got.get("updatedAt") or other.get("updatedAt")
                cell = _cell(_windows(got), at / 1000 if at else None, now)
            rows.append([labels[a["id"]] + (" *" if a["id"] == active else ""), cell])
        extra.append({"name": f"{title} 계정", "rows": rows[:6], "sub": "* ORCA 에서 지금 쓰는 계정"})
    return main, extra
