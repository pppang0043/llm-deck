"""한도 사용률을 비공개 GitHub gist 에 올려 여러 PC 가 함께 쓴다.

Claude Code / Codex 가 있는 PC 는 새 값을 올리고, 없는 PC 는 내려받아 보여준다.
서비스마다 가장 최근 시각(as_of)의 값이 이긴다. config.toml 의 [sync] 가 비어 있으면 아무것도 하지 않는다.
"""

import json
import os
import subprocess
import time
import urllib.error
import urllib.request

API = "https://api.github.com/gists"
FILE = "llm-deck.json"
FETCH_INTERVAL = 60
# 숫자는 그대로이고 시각만 바뀐 경우, 이만큼 지나야 다시 올린다 (쓸 때마다 올리지 않도록)
PUSH_INTERVAL = 300


def _request(method: str, url: str, token: str, body: dict | None = None) -> dict:
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode() if body is not None else None,
        method=method,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "llm-deck",
        },
    )
    with urllib.request.urlopen(req, timeout=10) as res:
        return json.load(res)


def _token(cfg: dict) -> str:
    """설정 파일 > 환경 변수 > GitHub CLI(gh) 로그인 순으로 토큰을 찾는다."""
    token = cfg.get("github_token") or os.environ.get("GITHUB_TOKEN", "")
    if token:
        return token
    try:
        return subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def create(cfg: dict) -> None:
    """공유용 gist 를 새로 만들고 id 를 알려준다."""
    token = _token(cfg)
    if not token:
        print("[sync] GitHub 토큰이 없습니다. `gh auth login -s gist` 로 로그인하거나 config.toml 의 [sync] github_token 을 채우세요.")
        return
    gist = _request(
        "POST", API, token, {"description": "LLM Deck usage sync", "public": False, "files": {FILE: {"content": "{}"}}}
    )
    print(f'[sync] gist 를 만들었습니다. 모든 PC 의 config.toml [sync] 에 적으세요:\n    gist_id = "{gist["id"]}"')


def newest(a: dict | None, b: dict | None) -> dict | None:
    if not a or not b:
        return a or b
    return a if a.get("as_of", 0) >= b.get("as_of", 0) else b


def _worth_pushing(new: dict, old: dict | None) -> bool:
    if not old or {**new, "as_of": 0} != {**old, "as_of": 0}:
        return True
    return new.get("as_of", 0) - old.get("as_of", 0) >= PUSH_INTERVAL


class Sync:
    def __init__(self, cfg: dict):
        self.gist_id = cfg.get("gist_id", "")
        self.token = _token(cfg)
        self.owner = ""
        self.remote: dict = {}
        self.fetched_at = 0.0
        self.failed = False

    @property
    def enabled(self) -> bool:
        return bool(self.gist_id and self.token)

    def _call(self, method: str, body: dict | None = None) -> dict | None:
        try:
            result = _request(method, f"{API}/{self.gist_id}", self.token, body)
            self.failed = False
            return result
        except (urllib.error.URLError, OSError, ValueError) as e:
            if not self.failed:  # 인터넷이 없을 때 같은 말을 반복하지 않는다
                print(f"[sync] GitHub 에 연결하지 못했습니다: {e}")
            self.failed = True
            return None

    def _fetch(self) -> None:
        if time.time() - self.fetched_at < FETCH_INTERVAL:
            return
        self.fetched_at = time.time()
        gist = self._call("GET")
        if gist is None:
            return
        self.owner = (gist.get("owner") or {}).get("login", "")
        try:
            remote = json.loads(gist["files"][FILE]["content"])
            self.remote = remote if isinstance(remote, dict) else {}
        except (KeyError, ValueError, TypeError):
            self.remote = {}

    @property
    def public_url(self) -> str:
        """로그인 없이 읽을 수 있는 주소. 기기가 PC 없이 직접 읽을 때 쓴다."""
        if not self.owner:
            return ""
        return f"https://gist.githubusercontent.com/{self.owner}/{self.gist_id}/raw/{FILE}"

    def merge(self, local: dict[str, dict | None]) -> dict[str, dict]:
        """이 PC 의 값과 공유된 값을 합쳐 돌려준다. 이 PC 의 값이 더 새로우면 올린다."""
        if not self.enabled:
            return {k: v for k, v in local.items() if v}
        self._fetch()
        merged = {k: newest(local.get(k), self.remote.get(k)) for k in [*local, *(k for k in self.remote if k not in local)]}
        merged = {k: v for k, v in merged.items() if v}
        if any(_worth_pushing(v, self.remote.get(k)) for k, v in merged.items()):
            if self._call("PATCH", {"files": {FILE: {"content": json.dumps(merged, indent=1)}}}) is not None:
                self.remote = merged
                print(f"[sync] 새 값을 올렸습니다 ({', '.join(sorted(merged))})")
        return merged
