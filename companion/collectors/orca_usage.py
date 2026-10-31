"""현재 PC의 ORCA가 선택한 계정의 한도. 인증 정보나 다른 PC의 값은 읽지 않는다."""

import hashlib
import math
import subprocess
import json

import orca
from collectors.fmt import limits_page


def read() -> dict | None:
    command = orca.find_orca()
    if not command:
        return None
    try:
        result = subprocess.run(
            [command, "account", "list", "--json"], capture_output=True,
            text=True, encoding="utf-8", timeout=10,
            creationflags=orca._NO_WINDOW,
        )
        if result.returncode:
            return None
        envelope = json.loads(result.stdout)
        data = envelope.get("result")
        if envelope.get("ok") and isinstance(data, dict) and isinstance(data.get("rateLimits"), dict):
            return data
    except (OSError, subprocess.SubprocessError, ValueError, TypeError, AttributeError):
        pass
    return None


def pages(data: dict, cfg: dict) -> dict[str, dict]:
    """선택 계정만 표시한다. 실패한 계정에 기본 CLI/공유 계정의 값을 대신 넣지 않는다."""
    result = {}
    for provider, section, title in (("claude", "claude_code", "Claude"), ("codex", "codex", "Codex")):
        if not cfg.get(section, {}).get("enabled", True):
            continue
        state = data.get(provider) or {}
        target = data["rateLimits"].get(provider + "Target") or {}
        # 현재 호스트만 대상. WSL 값은 호스트의 기본 계정으로 오인하지 않는다.
        if target.get("runtime", "host") != "host":
            result[section] = {"name": title, "headline": "-", "sub": "ORCA 사용량 대상이 현재 PC가 아닙니다."}
            continue
        active = state.get("activeAccountIdsByRuntime", {}).get("host", state.get("activeAccountId"))
        account = next((a for a in state.get("accounts", []) if a.get("id") == active), {}) if active else {}
        default = state.get("systemDefault") or {}
        email = account.get("email") if active else default.get("email") or state.get("systemDefaultEmail")
        identity = active or email or "system"
        suffix = hashlib.sha256(str(identity).encode()).hexdigest()[:8]
        name = f"ORCA {title} {suffix}"  # 기존 CLI 계정 및 계정 전환 시 그래프·소진 예측도 분리한다
        usage = data["rateLimits"].get(provider) or {}
        windows = []
        for key, label in (("session", "5h"), ("weekly", "Week")):
            window = usage.get(key)
            if not isinstance(window, dict):
                continue
            pct = window.get("usedPercent")
            if not isinstance(pct, (int, float)) or not math.isfinite(pct):
                continue
            resets = window.get("resetsAt")
            windows.append([label, pct, resets / 1000 if resets else None])
        updated = usage.get("updatedAt")
        if windows and isinstance(updated, (int, float)) and updated > 0:
            page = limits_page(name, {"as_of": updated / 1000, "windows": windows})
            page["rows"] = [["계정", email or str(identity)], ["기준", "이 PC의 ORCA 선택 계정"]]
            if usage.get("error"):
                page["sub"] = "ORCA 갱신 실패: 마지막 수집값"
        else:
            page = {"name": name, "headline": "-", "sub": "ORCA에서 사용량을 확인해 주세요.",
                    "rows": [["계정", email or str(identity)]]}
        page["title"] = f"ORCA {title}"
        result[section] = page
    return result
