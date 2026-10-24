"""ChatGPT 구독 계정으로 받아 적기 (ChatGPT 앱의 받아쓰기가 쓰는 곳). API 키가 필요 없다.

Codex CLI 에 ChatGPT 계정으로 로그인해 두면 ~/.codex/auth.json 에 남는 로그인 정보를 그대로 쓴다.
로그인 정보는 읽기만 한다 (갱신은 Codex 가 한다. 여기서 갱신하면 Codex 로그인이 풀릴 수 있다).
공개된 API 가 아니라 ChatGPT 웹이 쓰는 내부 주소라서 예고 없이 막힐 수 있다. 실패하면 None 을 돌려주고,
부르는 쪽(assistant.py)이 이 PC 안의 Whisper 로 받아 적는다.
소리는 OpenAI 로 나가고, OpenAI 쪽에 30일 동안 남는다 (응답의 asset_ttl).
"""

import io
import json
import os
import urllib.error
import urllib.request
import uuid
import wave
from pathlib import Path

URL = "https://chatgpt.com/backend-api/transcribe"
TIMEOUT = 20
# 브라우저가 보내는 것과 같은 머리글 (없으면 막힌다)
USER_AGENT = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36")


def _auth_file() -> Path:
    return Path(os.environ.get("CODEX_HOME") or Path.home() / ".codex") / "auth.json"


def _wav(pcm, rate: int) -> bytes:
    """float32 (-1~1) 소리를 16비트 모노 wav 로."""
    import numpy as np

    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((np.clip(pcm, -1, 1) * 32767).astype("<i2").tobytes())
    return out.getvalue()


def text(pcm, rate: int, language: str) -> str | None:
    """받아 적은 글. 로그인 정보가 없거나 막히면 None (이유는 기록에 남긴다)."""
    try:
        tokens = json.loads(_auth_file().read_text(encoding="utf-8"))["tokens"]
        token = tokens["access_token"]
    except (OSError, ValueError, KeyError, TypeError):
        print(f"[assistant] ChatGPT 로그인 정보가 없습니다 ({_auth_file()}). 'codex login' 으로 ChatGPT 계정에 로그인하세요")
        return None
    boundary = uuid.uuid4().hex
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="audio.wav"\r\n'
            "Content-Type: audio/wav\r\n\r\n").encode() + _wav(pcm, rate)
    if language and language != "auto":
        body += f'\r\n--{boundary}\r\nContent-Disposition: form-data; name="language"\r\n\r\n{language}'.encode()
    body += f"\r\n--{boundary}--\r\n".encode()
    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "Accept": "application/json",
        "Origin": "https://chatgpt.com",
        "Referer": "https://chatgpt.com/",
        "User-Agent": USER_AGENT,
    }
    if tokens.get("account_id"):
        headers["ChatGPT-Account-ID"] = tokens["account_id"]
    try:
        with urllib.request.urlopen(urllib.request.Request(URL, body, headers), timeout=TIMEOUT) as r:
            return str(json.loads(r.read())["text"]).strip()
    except urllib.error.HTTPError as e:
        why = {401: "로그인이 만료됨. Codex 를 한 번 쓰거나 'codex login' 을 다시 하세요",
               403: "막힘 (주소가 바뀌었을 수 있음)", 429: "너무 자주 씀"}.get(e.code, "")
        print(f"[assistant] ChatGPT 받아쓰기 실패 (HTTP {e.code}) {why}")
    except (OSError, ValueError, KeyError) as e:
        print(f"[assistant] ChatGPT 받아쓰기 실패: {e}")
    return None
