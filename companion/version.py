"""도우미와 기기 펌웨어의 버전 맞추기, 기기 설정 > 정보 에 보여줄 도우미 상태.

PROTO 는 서로 주고받는 내용(메시지 칸, 인증 방식)이 바뀔 때 하나씩 올린다. 펌웨어 link.h 의 FW_PROTO 와 같아야 한다.
    1: 이 번호를 주고받기 전 (보내지 않으면 0 으로 본다)
    2: Wi-Fi 인증, 키워드 버튼, ORCA 답장, 도우미 상태("helper")
"""

import subprocess
from pathlib import Path

PROTO = 2
# git 이 없거나 저장소 밖에서 돌 때 보여줄 버전
VERSION = "proto2"
# 기기가 버전을 알려 주지 않은 채 사용량을 이만큼 보내면 오래된 펌웨어로 본다
SILENT_SENDS = 3

_version: str | None = None


def version() -> str:
    """사람이 읽는 도우미 버전: git 커밋 (예: 430be27, 고친 것이 있으면 430be27-dirty). 없으면 VERSION."""
    global _version
    if _version is None:
        try:
            out = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=Path(__file__).parent,
                                 capture_output=True, text=True, timeout=3)
            _version = out.stdout.strip()[:23] if out.returncode == 0 else ""
        except (OSError, subprocess.SubprocessError):
            _version = ""
        _version = _version or VERSION
    return _version


def short_error(errors: dict[str, str]) -> str:
    """수집 오류들을 한 줄로 (예: "codex: 로그인 만료 외 1건"). 없으면 ""."""
    if not errors:
        return ""
    section, text = next(iter(errors.items()))
    line = f"{section}: {' '.join(text.split())}"
    if len(line) > 48:
        line = line[:47] + "~"
    return line + (f" 외 {len(errors) - 1}건" if len(errors) > 1 else "")


def status(collected_at: float, errors: dict[str, str]) -> dict:
    """사용량 메시지의 "helper" 칸. 작게 유지한다."""
    out = {"proto": PROTO, "ver": version(), "at": int(collected_at)}
    err = short_error(errors)
    if err:
        out["err"] = err
    return out


def compare(theirs: int) -> str:
    """기기 proto 와 비교한 안내 (같으면 "")."""
    if theirs < PROTO:
        return f"기기 펌웨어(proto {theirs})가 도우미(proto {PROTO})보다 오래되었습니다. 펌웨어를 올리세요."
    if theirs > PROTO:
        return f"도우미(proto {PROTO})가 기기 펌웨어(proto {theirs})보다 오래되었습니다. 도우미를 업데이트하세요 (git pull)."
    return ""


class Peer:
    """연결된 기기가 알려 준 펌웨어 버전({"fw_proto", "fw"})을 보고 다르면 한 번 기록한다 (연결마다)."""

    def __init__(self, log=print):
        self.log = log
        self.reset()

    def reset(self) -> None:
        self.proto: int | None = None
        self.sends = 0
        self.warned = False

    def on_event(self, event: dict) -> None:
        if not isinstance(event, dict) or "fw_proto" not in event:
            return
        try:
            proto = int(event["fw_proto"])
        except (TypeError, ValueError):
            return
        first = self.proto is None
        self.proto = proto
        if first:
            self.log(f"[version] 기기 펌웨어 {event.get('fw', '?')} (proto {proto}), 도우미 {version()} (proto {PROTO})")
        message = compare(proto)
        if message and not self.warned:
            self.warned = True
            self.log(f"[version] {message}")

    def sent(self) -> None:
        """사용량을 보낼 때마다 부른다. 기기가 끝내 답하지 않으면 proto 0 (오래된 펌웨어)."""
        self.sends += 1
        if self.proto is None and self.sends >= SILENT_SENDS and not self.warned:
            self.warned = True
            self.log(f"[version] 기기가 버전을 알려 주지 않습니다. {compare(0)}")

