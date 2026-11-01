"""OpenAI 공식 API 로 받아 적기. 키는 KEYS_FILE 에서 읽고 기록에 남기지 않는다.
소리는 OpenAI 로 나간다. 키가 없거나 실패하면 None 을 돌려준다.
"""

import json
import os
import urllib.error
import urllib.request
import uuid

from chatgpt_stt import TIMEOUT, _wav

URL = "https://api.openai.com/v1/audio/transcriptions"
MODEL = "gpt-4o-transcribe"
KEYS_FILE = "~/.secrets/stick_hub.env"


def load_keys(path: str) -> dict:
    """KEY=VALUE 파일을 읽는다. 값은 기록에 남기지 않는다."""
    keys = {}
    try:
        with open(os.path.expanduser(path), encoding="utf-8") as f:
            for line in f:
                name, eq, value = line.strip().partition("=")
                if eq and not name.startswith("#"):
                    keys[name.strip()] = value.strip().strip("\"'")
    except OSError:
        pass
    return keys


def text(pcm, rate: int, language: str) -> str | None:
    """받아 적은 글. 키가 없거나 막히면 None (이유는 기록에 남긴다)."""
    key = load_keys(KEYS_FILE).get("OPENAI_API_KEY")
    if not key:
        print(f"[assistant] OpenAI API 키가 없습니다 ({KEYS_FILE})")
        return None
    boundary = uuid.uuid4().hex
    fields = {"model": MODEL, "response_format": "json"}
    if language and language != "auto":
        fields["language"] = language
    body = b"".join(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode()
                    for name, value in fields.items())
    body += (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="audio.wav"\r\n'
             "Content-Type: audio/wav\r\n\r\n").encode() + _wav(pcm, rate) + f"\r\n--{boundary}--\r\n".encode()
    headers = {"Authorization": f"Bearer {key}", "Content-Type": f"multipart/form-data; boundary={boundary}"}
    try:
        with urllib.request.urlopen(urllib.request.Request(URL, body, headers), timeout=TIMEOUT) as r:
            return str(json.loads(r.read())["text"]).strip()
    except urllib.error.HTTPError as e:
        print(f"[assistant] OpenAI API 받아쓰기 실패 (HTTP {e.code})")
    except (OSError, ValueError, KeyError) as e:
        print(f"[assistant] OpenAI API 받아쓰기 실패: {e}")
    return None
