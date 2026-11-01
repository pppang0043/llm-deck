"""ORCA 세션 팝업 버튼의 배치·표시명·입력 글. 원본은 보드(NVS)다. 어느 PC 에 꽂아도 같고, PC 는 연결할 때 읽어 온다.

  PC -> 기기: {"pop_sync": true} 저장된 것을 보내라, {"pop_set": [{"id", "name", "text"} | {"rows": "a,b;c;d"}]} 저장 (null 은 기본값으로),
             {"pop": [[{"id", "name", "s"}, ...], [...], [...]]} 지금 있는 버튼에 맞춰 합친 표. s: 글이 / 로 시작한다
  기기 -> PC: {"orca": "edit", "key": id} 수정 요청, {"orca": "saved_item", "id", "name"?, "text"?, "ack"?},
             {"orca": "saved_row", "row", "ids": "a,b"}, {"orca": "saved_end"}. 위치 조정은 기기에서 저장한 뒤 알려 준다.
             {"orca": "new", "after": id} 그 버튼 뒤에 새 버튼. 입력은 수정과 같은 창에서 하고, id 는 u1, u2... ("new": true 로 저장)
  수정은 기기에 키보드가 없어 미러 창에서 한다: 미러 창이 GET /edit 로 요청을 읽어 입력 창을 띄우고 POST /edit 로 답한다 (mirror.py).
저장한 글은 터미널에 입력되므로 typer 의 키워드와 같은 규칙으로 거른다.
"""

import json
import re
import threading
import time
from pathlib import Path

import typer

ROWS = 3
MAX_BUTTONS = 20  # 기기 POP_MAX
SYNC_RETRY_S = 3  # 보드가 답이 없으면 이만큼 뒤 다시 묻는다
# 키워드가 아닌 버튼 (id, 표시명, 글). 글이 None 이면 글 없이 키만 누르는 버튼이라 표시명과 위치만 고친다.
# 글이 있는 버튼의 기본 글은 orca.py 의 KEYS, SKILLS 와 같게
SECOND_ROW = [("resume", "리슘", "/resume"), ("skill:proto-design", "proto", "/proto-design "),
              ("erase", "지우기", None), ("skills", "스킬", None)]
CUSTOM_ID = re.compile(r"u\d+")  # 새로 만든 버튼
THIRD_ROW = [("dictate", "녹음", None), ("tab", "Tab", None), ("tab_enter", "Tab+E", None),
             ("enter", "Enter", None), ("esc", "Esc", None)]


def _old_file() -> Path:
    """예전 도우미가 쓰던 PC 저장 파일 (보드로 옮긴다)"""
    return Path.home() / ".llm-deck" / "orca_buttons.json"


def _in_font(ch: str) -> bool:
    """기기 글꼴(font_ko_14)에 있는 글자: ASCII 출력 문자와 한글 완성형 2350자"""
    if " " <= ch <= "~":
        return True
    try:
        # 2350자 밖의 한글은 파이썬이 조합 표기로 8바이트를 낸다
        return "가" <= ch <= "힣" and len(ch.encode("euc-kr")) == 2
    except UnicodeEncodeError:
        return False


def name_error(name) -> str | None:
    """표시명이 쓸 수 없으면 이유. 앞뒤 띄어쓰기는 세지 않는다 (typer.configure 와 같다)"""
    if not isinstance(name, str) or not name.strip():
        return "표시명을 적어 주세요"
    if len(name.strip()) > typer.MAX_NAME:
        return f"표시명은 {typer.MAX_NAME}자까지입니다"
    wrong = next((c for c in name.strip() if not _in_font(c)), None)
    return f"기기 화면에 없는 글자입니다: {wrong}" if wrong else None


def text_error(text) -> str | None:
    """입력 글이 쓸 수 없으면 이유 (typer.configure 와 같은 규칙)"""
    if not isinstance(text, str) or not text.strip():
        return "입력 내용을 적어 주세요"
    if len(text) > typer.MAX_TEXT:
        return f"입력 내용은 {typer.MAX_TEXT}자까지입니다"
    return "입력 내용에 줄바꿈이나 탭은 넣을 수 없습니다" if typer._bad(text) else None


def _base() -> dict[str, tuple[str, str | None]]:
    """지금 쓸 수 있는 버튼: id -> (기본 표시명, 기본 글)"""
    return {kid: (name, text) for kid, name, text in typer.BASE_ENTRIES + SECOND_ROW + THIRD_ROW}


def default_rows() -> list[list[str]]:
    keys = [kid for kid, _, _ in typer.BASE_ENTRIES]
    orch = ["orchestration"] if "orchestration" in keys else []  # 기본 키워드 중 이것만 둘째 줄 맨 앞
    return [[k for k in keys if k != "orchestration"], orch + [b[0] for b in SECOND_ROW], [b[0] for b in THIRD_ROW]]


class PopButtons:
    def __init__(self) -> None:
        self.lock = threading.Lock()  # 기기 수신, 미러 HTTP, 메인 루프가 함께 쓴다
        self.sent: list | None = None
        self.edit: dict | None = None  # 미러 창에서 고치는 중인 요청
        self.seq = 0
        self.rows, self.names, self.texts = default_rows(), {}, {}
        self.synced = False  # 보드에서 저장된 것을 다 읽었다
        self.asked = 0.0
        self.incoming: dict = {"rows": [None] * ROWS, "names": {}, "texts": {}}
        self.outbox: list[dict] = []  # 보드에 저장할 것 {"pop_set"}
        self.custom: list[str] = []  # 새로 만든 버튼 (표시명과 글은 names, texts 에)

    def _all(self) -> dict[str, tuple[str, str | None]]:
        """_base 와 새로 만든 버튼. 새 버튼은 기본값이 없어 표시명과 글을 늘 저장한다"""
        return {**_base(), **{kid: ("", "") for kid in self.custom}}

    def _merge(self, raw_rows: list) -> list[list[str]]:
        """저장한 자리를 지금 있는 버튼에 맞춘다: 없는 id 는 버리고, 새 id 는 기본 자리에 끼운다"""
        base = self._all()
        rows: list[list[str]] = [[] for _ in range(ROWS)]
        seen = set()
        for row, raw in zip(rows, raw_rows):
            for kid in raw if isinstance(raw, list) else []:
                if isinstance(kid, str) and kid in base and kid not in seen:
                    seen.add(kid)
                    row.append(kid)
        for row, defaults in zip(rows, default_rows()):
            for index, kid in enumerate(defaults):
                if kid not in seen:
                    row.insert(index, kid)
        rows[1].extend(kid for kid in self.custom if kid not in seen)
        return rows

    def _message(self) -> list[list[dict]]:
        base = self._all()
        rows = []
        for row in self.rows:
            rows.append([{"id": kid, "name": self.names.get(kid, base[kid][0]),
                          "s": (self.texts.get(kid, base[kid][1]) or "").startswith("/")} for kid in row])
        return rows

    def pending(self) -> list | None:
        """기기로 보낼 {"pop"} 값 (보드에서 읽은 뒤, 바뀌었을 때만)"""
        with self.lock:
            if not self.synced:
                return None
            message = self._message()
            if message == self.sent:
                return None
            self.sent = message
            return message

    def commands(self) -> dict | None:
        """기기로 보낼 {"pop_sync"} (저장된 것을 읽어 온다. 답이 없으면 다시 묻는다) 나 {"pop_set"} (고친 것을 저장한다)"""
        with self.lock:
            if not self.synced:
                if time.monotonic() - self.asked < SYNC_RETRY_S:
                    return None
                self.asked = time.monotonic()
                self.incoming = {"rows": [None] * ROWS, "names": {}, "texts": {}}
                self.custom = []
                return {"pop_sync": True}
            if self.outbox:
                out, self.outbox = self.outbox, []
                return {"pop_set": out}
            return None

    def resend(self) -> None:
        """기기가 새로 연결됐다: 보드에서 다시 읽는다 (다른 PC 에서 고쳤을 수 있다)"""
        with self.lock:
            self.sent = None
            self.synced = False
            self.asked = 0.0
            self.edit = None
            self.outbox = []

    def text(self, kid: str) -> str | None:
        """사용자가 고친 글. 없으면 None (그때는 기본 글)"""
        return self.texts.get(kid)

    def on_saved_item(self, event: dict) -> bool:
        """보드가 알려 준 버튼 하나 {"id", "name"?, "text"?, "ack"?}. 없는 칸은 기본값이다. 저장의 답이면 True."""
        with self.lock:
            kid, name, text = event.get("id"), event.get("name"), event.get("text")
            if isinstance(kid, str) and CUSTOM_ID.fullmatch(kid) and not name_error(name) and not text_error(text):
                if kid not in self.custom:
                    self.custom.append(kid)
            base = self._all()
            if not isinstance(kid, str) or kid not in base:
                return False
            names = self.names if self.synced else self.incoming["names"]
            texts = self.texts if self.synced else self.incoming["texts"]
            names.pop(kid, None)
            texts.pop(kid, None)
            if name is not None and not name_error(name):
                names[kid] = name.strip()
            if text is not None and base[kid][1] is not None and not text_error(text):
                texts[kid] = text
            if not self.synced:
                return False
            typer.apply_saved(self.names, self.texts)
            return event.get("ack") is True

    def on_saved_row(self, event: dict) -> None:
        row, ids = event.get("row"), event.get("ids")
        with self.lock:
            if isinstance(row, int) and 0 <= row < ROWS and isinstance(ids, str):
                self.incoming["rows"][row] = [kid for kid in ids.split(",") if kid]

    def on_saved_end(self) -> None:
        """보드의 저장을 다 읽었다 (연결할 때, 또는 위치 조정을 끝냈을 때). 합친 표를 기기에 돌려준다."""
        with self.lock:
            got = self.incoming["rows"]
            if any(r is not None for r in got):
                self.rows = self._merge(got)
            elif not self.synced:
                self.rows = self._merge([])
            if not self.synced:
                self.names, self.texts = self.incoming["names"], self.incoming["texts"]
                self.synced = True
                self._move_old_file(any(r is not None for r in got))
            self.incoming = {"rows": [None] * ROWS, "names": {}, "texts": {}}
            typer.apply_saved(self.names, self.texts)

    def _move_old_file(self, board_has_layout: bool) -> None:
        """예전 도우미가 PC 파일에 저장한 것을 비어 있는 보드로 한 번 옮기고 파일 이름을 바꾼다"""
        path = _old_file()
        try:
            saved = json.loads(path.read_text(encoding="utf-8"))
            names, texts, rows = saved.get("names", {}), saved.get("texts", {}), saved.get("rows")
            if not board_has_layout and not self.names and not self.texts:
                base = _base()
                for kid in set(names) | set(texts):
                    if kid in base:
                        self.outbox.append({"id": kid, "name": names.get(kid), "text": texts.get(kid)})
                if rows:
                    self.outbox.append({"rows": ";".join(",".join(row) for row in rows)})
            path.rename(path.with_name(path.name + ".moved"))
        except FileNotFoundError:
            pass
        except (OSError, ValueError, AttributeError, TypeError) as e:
            print(f"[orca] {path} 을 보드로 옮기지 못했습니다: {e}")

    def begin_edit(self, kid) -> dict | None:
        """기기가 고쳐 달라고 한 버튼을 미러 창이 읽어 갈 요청으로 둔다. 없는 버튼이면 None."""
        with self.lock:
            base = self._all()
            if not isinstance(kid, str) or kid not in base:
                return None
            default_name, default_text = base[kid]
            self.seq += 1
            self.edit = {"seq": self.seq, "id": kid, "name": self.names.get(kid, default_name),
                         "text": self.texts.get(kid, default_text) or "", "has_text": default_text is not None}
            return dict(self.edit)

    def begin_new(self, after) -> dict | None:
        """기기가 after 버튼 뒤에 새 버튼을 만들어 달라고 했다. 빈 입력 창을 띄운다. 못 만들면 None."""
        with self.lock:
            if not self.synced or sum(map(len, self.rows)) >= MAX_BUTTONS or not any(after in r for r in self.rows):
                return None
            number = next(n for n in range(1, MAX_BUTTONS + 2) if f"u{n}" not in self.custom)
            self.seq += 1
            self.edit = {"seq": self.seq, "id": f"u{number}", "name": "", "text": "", "has_text": True,
                         "new": True, "after": after}
            return dict(self.edit)

    def current_edit(self) -> dict:
        with self.lock:
            return dict(self.edit) if self.edit else {}

    def answer(self, body) -> tuple[int, dict]:
        """미러 창의 답 {seq, id, name, text} 또는 {seq, cancel: true}. (HTTP 코드, 답).
        저장은 보드에 보내고, 보드가 저장했다고 답하면 (on_saved_item) 반영한다."""
        with self.lock:
            edit = self.edit
            if not isinstance(body, dict) or not edit or body.get("seq") != edit["seq"]:
                return 409, {"error": "다른 수정 요청으로 바뀌었습니다"}
            if body.get("cancel") is True:
                self.edit = None
                return 200, {}
            kid = edit["id"]
            if body.get("id") != kid:
                return 400, {"error": "수정하던 버튼이 아닙니다"}
            name, text = body.get("name"), body.get("text", "")
            error = name_error(name)
            if not error and edit["has_text"]:
                error = text_error(text)
            elif not error and text not in ("", None):
                error = "이 버튼에는 입력 내용을 넣을 수 없습니다"
            if error:
                return 400, {"error": error}
            if not self.synced:
                return 409, {"error": "기기가 연결되어 있지 않습니다"}
            if edit.get("new"):
                rows = [[*r] for r in self.rows]
                row = next(r for r in rows if edit["after"] in r)
                row.insert(row.index(edit["after"]) + 1, kid)
                self.outbox += [{"id": kid, "name": name.strip(), "text": text, "new": True},
                                {"rows": ";".join(",".join(r) for r in rows)}]
                self.edit = None
                return 200, {}
            default_name, default_text = self._all()[kid]
            # 기본값과 같으면 null: 보드에서 지운다
            self.outbox.append({"id": kid, "name": None if name.strip() == default_name else name.strip(),
                                "text": None if not edit["has_text"] or text == default_text else text})
            self.edit = None
            return 200, {}
