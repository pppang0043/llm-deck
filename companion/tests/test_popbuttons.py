import http.client
import json
import re
import threading
from pathlib import Path

import pytest

import mirror
import orca
import popbuttons
import typer

FONT = Path(__file__).resolve().parents[2] / "firmware" / "src" / "fonts" / "font_ko_14.c"
DEFAULT_ROWS = [
    ["commit", "deploy", "push", "error", "clear", "compact"],
    ["orchestration", "resume", "skill:proto-design", "erase", "skills"],
    ["dictate", "tab", "tab_enter", "enter", "esc"],
]


@pytest.fixture(autouse=True)
def clean_keywords(fake_home):  # 진짜 ~/.llm-deck 의 옛 파일을 건드리지 않게 fake_home 을 함께 쓴다
    typer.configure(None)
    yield
    typer.configure(None)


@pytest.fixture
def o(monkeypatch):
    inst = orca.Orca({"enabled": False})  # 읽는 스레드는 띄우지 않는다
    inst.sent_cmds = []
    inst.handles = {"abc": "term_abc"}
    inst._run = lambda *a, **k: inst.sent_cmds.append(a) or {}
    monkeypatch.setattr(orca.time, "sleep", lambda s: None)
    inst.opened = []
    inst.open_editor = lambda: inst.opened.append(1)
    inst.on_event({"orca": "saved_end"})  # 보드에 저장된 것이 없다
    return inst


def fresh() -> popbuttons.PopButtons:
    buttons = popbuttons.PopButtons()
    buttons.on_saved_end()
    return buttons


class Board:
    """보드의 NVS 처럼 저장하고 답한다"""

    def __init__(self):
        self.names, self.texts, self.layout = {}, {}, ""

    def take(self, inst):
        """도우미가 보낼 pop_set 을 저장하고 저장한 것을 돌려준다"""
        for item in (inst.pending_pop_commands() or {}).get("pop_set", []):
            if "rows" in item:
                self.layout = item["rows"]
                self.send_layout(inst)
                continue
            for store, key in ((self.names, "name"), (self.texts, "text")):
                store.pop(item["id"], None)
                if item[key] is not None:
                    store[item["id"]] = item[key]
            inst.on_event(self.item(item["id"], ack=True))

    def item(self, kid, ack=False):
        event = {"orca": "saved_item", "id": kid}
        if kid in self.names:
            event["name"] = self.names[kid]
        if kid in self.texts:
            event["text"] = self.texts[kid]
        return {**event, "ack": True} if ack else event

    def send_layout(self, inst):
        if self.layout:
            for row, ids_text in enumerate(self.layout.split(";")):
                inst.on_event({"orca": "saved_row", "row": row, "ids": ids_text})
        inst.on_event({"orca": "saved_end"})

    def sync(self, inst):
        for kid in {*self.names, *self.texts}:
            inst.on_event(self.item(kid))
        self.send_layout(inst)


def ids(rows):
    return [[b["id"] for b in row] for row in rows]


def begin(o, key):
    o.on_event({"orca": "edit", "key": key})
    return o.current_edit()


def save(o, req, **fields):
    """미러 창의 저장. 보드가 저장하고 답한 뒤까지"""
    reply = o.answer_edit({"seq": req["seq"], "id": req["id"], "name": req["name"], "text": req["text"], **fields})
    if reply[0] == 200:
        o.__dict__.setdefault("board", Board()).take(o)
    return reply


# --- 기본값과 보내기 ---


def test_default_layout_and_message():
    pop = fresh().pending()
    assert ids(pop) == DEFAULT_ROWS
    by_id = {b["id"]: b for row in pop for b in row}
    assert by_id["commit"] == {"id": "commit", "name": "커밋", "s": False}
    assert by_id["orchestration"]["s"] and by_id["clear"]["s"] and by_id["resume"]["s"]
    assert by_id["skill:proto-design"] == {"id": "skill:proto-design", "name": "proto", "s": True}
    assert by_id["tab_enter"] == {"id": "tab_enter", "name": "Tab+E", "s": False}
    assert sum(map(len, pop)) == 16  # 기기 POP_MAX


def test_defaults_match_orca_keys():
    for kid, _, text in popbuttons.SECOND_ROW:
        if kid in orca.KEYS:
            assert orca.KEYS[kid][1] == text or text is None
    assert "proto-design" in orca.SKILLS


def test_config_keywords_replace_row_zero():
    typer.configure({"items": [{"name": "하나", "text": "하나요"}, {"name": "둘", "text": "/둘"}]})
    pop = fresh().pending()
    assert ids(pop)[0] == ["k0", "k1"]
    assert ids(pop)[1] == ["resume", "skill:proto-design", "erase", "skills"]
    assert pop[0][1]["s"] is True


def test_pending_only_when_changed_or_resend():
    buttons = fresh()
    assert buttons.pending()
    assert buttons.pending() is None
    buttons.resend()
    assert buttons.pending() is None  # 보드에서 다시 읽기 전에는 보내지 않는다
    assert buttons.commands() == {"pop_sync": True}
    buttons.on_saved_end()
    assert buttons.pending()


def test_orca_resend_also_resends_pop(o):
    assert o.pending_pop()
    assert o.pending_pop() is None
    o.resend()
    assert o.pending_pop_commands() == {"pop_sync": True}
    o.on_event({"orca": "saved_end"})
    assert o.pending_pop()


# --- 보드에 저장 ---


def test_new_connection_asks_board_and_retries(monkeypatch):
    now = [100.0]
    monkeypatch.setattr(popbuttons.time, "monotonic", lambda: now[0])
    buttons = popbuttons.PopButtons()
    assert buttons.pending() is None
    assert buttons.commands() == {"pop_sync": True}
    assert buttons.commands() is None
    now[0] += popbuttons.SYNC_RETRY_S
    assert buttons.commands() == {"pop_sync": True}


def test_settings_come_from_board_after_restart(o):
    board = Board()
    board.names, board.texts = {"commit": "올리기"}, {"commit": "올려줘 "}
    board.layout = "esc,commit;;" + ",".join(b for r in DEFAULT_ROWS for b in r if b not in ("esc", "commit"))
    typer.configure(None)
    inst = orca.Orca({"enabled": False})
    assert typer.KEYWORDS["commit"] == "커밋해줘"  # 보드를 읽기 전
    assert inst.pending_pop_commands() == {"pop_sync": True}
    board.sync(inst)
    assert typer.KEYWORDS["commit"] == "올려줘 "
    assert next(i for i in typer.ITEMS if i["id"] == "commit")["name"] == "올리기"
    pop = inst.pending_pop()
    assert ids(pop)[0] == ["esc", "commit"] and pop[0][1]["name"] == "올리기"


def test_other_pc_edits_replace_after_reconnect(o):
    board = Board()
    assert save(o, begin(o, "commit"), name="올리기", text="올려줘")[0] == 200
    o.board.names.clear()
    o.board.texts.clear()  # 다른 PC 에서 기본값으로 되돌렸다
    o.resend()
    assert o.pending_pop_commands() == {"pop_sync": True}
    board.sync(o)
    assert o.buttons.names == {} and o.buttons.texts == {}
    assert typer.KEYWORDS["commit"] == "커밋해줘"


def test_unknown_ids_dropped_new_ids_inserted_at_default_place():
    buttons = popbuttons.PopButtons()
    buttons.on_saved_row({"row": 0, "ids": "esc,gone,esc"})
    buttons.on_saved_row({"row": 1, "ids": "commit"})
    buttons.on_saved_row({"row": 2, "ids": ""})
    buttons.on_saved_end()
    assert ids(buttons.pending()) == [["esc", "deploy", "push", "error", "clear", "compact"],
                                      ["orchestration", "resume", "skill:proto-design", "erase", "skills", "commit"],
                                      ["dictate", "tab", "tab_enter", "enter"]]


def test_bad_saved_values_ignored():
    buttons = popbuttons.PopButtons()
    for event in ({"id": "commit", "name": "x" * 13, "text": "줄\n바꿈"}, {"id": "push", "name": "😀", "text": "밀어줘"},
                  {"id": "gone", "name": "없음", "text": "글"}, {"id": "tab", "name": "탭", "text": "버튼에 글"}, {"id": 3}):
        buttons.on_saved_item(event)
    buttons.on_saved_row({"row": 9, "ids": "a"})
    buttons.on_saved_row({"row": 0, "ids": 3})
    buttons.on_saved_end()
    assert buttons.names == {"tab": "탭"}
    assert buttons.texts == {"push": "밀어줘"}
    assert typer.KEYWORDS["push"] == "밀어줘" and typer.KEYWORDS["commit"] == "커밋해줘"


def test_old_pc_file_moves_to_empty_board(fake_home):
    old = fake_home / ".llm-deck" / "orca_buttons.json"
    old.parent.mkdir()
    old.write_text(json.dumps({"rows": [["compact"], ["esc"], []], "names": {"compact": "압축", "gone": "없음"},
                               "texts": {"commit": "올려줘"}}), encoding="utf-8")
    buttons = popbuttons.PopButtons()
    buttons.on_saved_end()
    out = {json.dumps(i, sort_keys=True) for i in buttons.commands()["pop_set"]}
    assert out == {json.dumps(i, sort_keys=True) for i in (
        {"id": "compact", "name": "압축", "text": None}, {"id": "commit", "name": None, "text": "올려줘"},
        {"rows": "compact;esc;"})}
    assert not old.exists() and old.with_name(old.name + ".moved").exists()


def test_old_pc_file_does_not_overwrite_board(fake_home):
    old = fake_home / ".llm-deck" / "orca_buttons.json"
    old.parent.mkdir()
    old.write_text(json.dumps({"names": {"compact": "압축"}}), encoding="utf-8")
    buttons = popbuttons.PopButtons()
    buttons.on_saved_item({"id": "commit", "name": "보드"})
    buttons.on_saved_end()
    assert buttons.commands() is None
    assert not old.exists()


@pytest.mark.parametrize("broken", ["{깨짐", "[]", "1"])
def test_broken_old_file_is_left_alone(fake_home, broken, capsys):
    old = fake_home / ".llm-deck" / "orca_buttons.json"
    old.parent.mkdir()
    old.write_text(broken, encoding="utf-8")
    assert ids(fresh().pending()) == DEFAULT_ROWS
    assert "옮기지 못했습니다" in capsys.readouterr().out


def test_save_refused_while_board_not_read(o):
    req = begin(o, "commit")
    o.buttons.synced = False
    code, reply = o.answer_edit({"seq": req["seq"], "id": "commit", "name": "새", "text": "새 글"})
    assert code == 409 and reply["error"]


def test_saved_only_after_board_answers(o):
    req = begin(o, "commit")
    assert o.answer_edit({"seq": req["seq"], "id": "commit", "name": "새 이름", "text": "커밋해줘"}) == (200, {})
    assert o.buttons.names == {} and o.pending_toast != "저장했습니다"  # 보드 답을 기다린다
    assert o.pending_pop_commands() == {"pop_set": [{"id": "commit", "name": "새 이름", "text": None}]}
    o.on_event({"orca": "saved_item", "id": "commit", "name": "새 이름", "ack": True})
    assert o.buttons.names == {"commit": "새 이름"} and o.pending_toast == "저장했습니다"


# --- 수정 ---


def test_edit_event_without_session_id_opens_window(o):
    req = begin(o, "skill:proto-design")
    assert req == {"seq": req["seq"], "id": "skill:proto-design", "name": "proto", "text": "/proto-design ", "has_text": True}
    assert o.opened == [1]
    assert o.pending_toast == "PC 미러 창에서 수정하세요"


def test_edit_textless_button_has_no_text(o):
    req = begin(o, "esc")
    assert req["has_text"] is False and req["text"] == "" and req["name"] == "Esc"


@pytest.mark.parametrize("key", ["gone", "", None, 3, ["commit"]])
def test_edit_unknown_button_refused(o, key):
    o.pending_pop()
    assert begin(o, key) == {}
    assert o.opened == []
    assert "모르는" in o.pending_toast
    assert o.pending_pop_commands() == {"pop_sync": True}  # 보드에서 다시 읽어 기기를 맞춘다


def test_cancel_clears_request_and_new_request_changes_seq(o):
    first = begin(o, "push")
    second = begin(o, "deploy")
    assert second["seq"] != first["seq"]
    assert o.answer_edit({"seq": first["seq"], "cancel": True})[0] == 409
    assert o.answer_edit({"seq": second["seq"], "cancel": True}) == (200, {})
    assert o.current_edit() == {}
    assert o.pending_toast != "저장했습니다"


@pytest.mark.parametrize("body", [None, [], "x", 1, {}, {"seq": 999, "id": "push", "name": "a", "text": "b"}])
def test_wrong_seq_conflicts(o, body):
    begin(o, "push")
    assert o.answer_edit(body)[0] == 409
    assert o.current_edit()


def test_no_request_conflicts(o):
    assert o.answer_edit({"seq": 1, "cancel": True})[0] == 409


@pytest.mark.parametrize("fields", [
    {"id": "deploy"},
    {"name": ""}, {"name": "   "}, {"name": None}, {"name": 3}, {"name": "가" * 13},
    {"name": "😀"}, {"name": "한\t글"}, {"name": "ㅋㅋ"}, {"name": "똠"}, {"name": "日本"},
    {"text": ""}, {"text": "   "}, {"text": None}, {"text": 3}, {"text": "가" * 201},
    {"text": "줄\n바꿈"}, {"text": "탭\t"}, {"text": "\x1b[A"}, {"text": "\x7f"},
])
def test_bad_input_rejected_with_reason(o, fields):
    req = begin(o, "commit")
    code, reply = save(o, req, **fields)
    assert code == 400 and reply["error"]
    assert o.buttons.names == {} and o.buttons.texts == {}
    assert typer.KEYWORDS["commit"] == "커밋해줘"
    assert o.current_edit() == req  # 창에서 고쳐 다시 보낼 수 있다


def test_text_for_textless_button_rejected(o):
    req = begin(o, "tab")
    code, reply = save(o, req, text="몰래 넣기")
    assert code == 400 and reply["error"]
    assert save(o, begin(o, "tab"), name="탭", text="")[0] == 200
    assert o.buttons.names == {"tab": "탭"} and o.buttons.texts == {}


def test_name_limits_and_trim(o):
    assert save(o, begin(o, "push"), name=" 가 나1! ")[0] == 200
    assert o.buttons.names["push"] == "가 나1!"
    assert save(o, begin(o, "push"), name="가" * 12)[0] == 200


def test_text_limit_and_keeps_trailing_space(o):
    assert save(o, begin(o, "push"), text="가" * 200)[0] == 200
    assert save(o, begin(o, "push"), text="이어 쓰기 ")[0] == 200
    assert o.buttons.texts["push"] == "이어 쓰기 "


def test_only_differences_are_saved(o):
    assert save(o, begin(o, "commit"), name="올리기", text="올려줘")[0] == 200
    assert save(o, begin(o, "commit"), name="커밋", text="커밋해줘")[0] == 200
    assert o.board.names == {} and o.board.texts == {}
    assert typer.KEYWORDS["commit"] == "커밋해줘"


def test_save_resends_pop_and_toasts(o):
    o.pending_pop()
    assert save(o, begin(o, "commit"), name="올리기", text="/올려")[0] == 200
    assert o.pending_toast == "저장했습니다"
    assert o.current_edit() == {}
    button = o.pending_pop()[0][0]
    assert button == {"id": "commit", "name": "올리기", "s": True}
    assert next(i for i in typer.ITEMS if i["id"] == "commit")["name"] == "올리기"


def test_name_check_matches_device_font():
    # 글꼴은 ASCII 와 한글 2350자. 없는 글자를 받으면 기기에 네모로 나온다
    source = FONT.read_text(encoding="utf-8")
    body = re.sub(r"/\*.*?\*/", "", re.search(r"unicode_list_1\[\] = \{(.*?)\}", source, re.S).group(1), flags=re.S)
    glyphs = {0xAC00 + int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body)}
    assert {c for c in range(0xAC00, 0xD7A4) if popbuttons._in_font(chr(c))} == glyphs
    assert all(popbuttons._in_font(chr(c)) for c in range(32, 127))
    assert not popbuttons._in_font("\x7f") and not popbuttons._in_font("é")


# --- send_key 의 글 ---


@pytest.mark.parametrize("key", ["resume", "skill:proto-design", "commit"])
def test_send_key_uses_saved_text(o, key):
    assert save(o, begin(o, key), text="고친 글")[0] == 200
    o.send_key("abc", key)
    assert o.sent_cmds == [("terminal", "send", "--terminal", "term_abc", "--text", "고친 글"),
                           ("terminal", "send", "--terminal", "term_abc", "--enter")]


def test_send_key_trailing_space_skips_enter(o):
    assert save(o, begin(o, "resume"), text="/resume 이어서 ")[0] == 200
    o.send_key("abc", "resume")
    assert o.sent_cmds == [("terminal", "send", "--terminal", "term_abc", "--text", "/resume 이어서 ")]


def test_send_key_defaults_unchanged(o):
    o.send_key("abc", "skill:proto-design")
    assert o.sent_cmds == [("terminal", "send", "--terminal", "term_abc", "--text", "/proto-design ")]


def test_typer_uses_saved_keyword(o, monkeypatch):
    calls = []
    monkeypatch.setattr(typer, "_type", lambda text, enter=True, tab=False: calls.append((text, enter)) or None)
    assert save(o, begin(o, "push"), text="올려 ")[0] == 200
    typer.Typer().on_event({"type": "push"})
    assert calls == [("올려 ", False)]


# --- 신규 ---


def make(o, after, name, text):
    o.on_event({"orca": "new", "after": after})
    req = o.current_edit()
    return req, save(o, req, name=name, text=text)


def test_new_button_goes_after_pressed_one_and_works(o):
    req, reply = make(o, "commit", "테스트", "테스트 돌려줘")
    assert reply == (200, {}) and req["id"] == "u1" and req["new"] and req["has_text"] and o.opened == [1]
    assert (req["name"], req["text"]) == ("", "")
    assert o.pending_toast == "저장했습니다"
    pop = o.pending_pop()
    assert ids(pop)[0][:2] == ["commit", "u1"]
    assert pop[0][1] == {"id": "u1", "name": "테스트", "s": False}
    assert o.board.layout.startswith("commit,u1,deploy")
    o.sent_cmds.clear()
    o.send_key("abc", "u1")
    assert o.sent_cmds == [("terminal", "send", "--terminal", "term_abc", "--text", "테스트 돌려줘"),
                           ("terminal", "send", "--terminal", "term_abc", "--enter")]


def test_new_button_survives_reconnect_and_next_gets_new_id(o):
    make(o, "esc", "하나", "/하나")
    o.resend()
    assert o.pending_pop_commands() == {"pop_sync": True}
    assert o.buttons.custom == []
    o.board.sync(o)
    assert o.buttons.custom == ["u1"] and ids(o.pending_pop())[2][-1] == "u1"
    req, _ = make(o, "u1", "둘", "둘째")
    assert req["id"] == "u2" and ids(o.pending_pop())[2][-2:] == ["u1", "u2"]


def test_new_button_edit_keeps_its_values(o):
    make(o, "commit", "하나", "하나요")
    req = begin(o, "u1")
    assert (req["name"], req["text"], req["has_text"]) == ("하나", "하나요", True)
    assert save(o, req, name="둘", text="둘이요")[0] == 200
    assert o.board.names["u1"] == "둘" and o.buttons.text("u1") == "둘이요"


def test_new_button_needs_name_and_text(o):
    o.on_event({"orca": "new", "after": "commit"})
    req = o.current_edit()
    assert save(o, req, name="", text="글")[0] == 400
    assert save(o, req, name="이름", text="")[0] == 400
    assert o.current_edit()  # 다시 입력한다


@pytest.mark.parametrize("after", ["gone", None, 3])
def test_new_after_unknown_refused(o, after):
    o.on_event({"orca": "new", "after": after})
    assert o.current_edit() == {} and o.opened == []


def test_button_limit(o):
    for n in range(popbuttons.MAX_BUTTONS - 16):
        assert make(o, "commit", f"새{n}", "글")[1][0] == 200
    assert sum(map(len, o.buttons.rows)) == popbuttons.MAX_BUTTONS
    o.pending_toast = None
    o.on_event({"orca": "new", "after": "commit"})
    assert o.current_edit() == {} and f"{popbuttons.MAX_BUTTONS}개" in o.pending_toast


def test_unknown_custom_id_without_values_ignored():
    buttons = popbuttons.PopButtons()
    for event in ({"id": "u1", "name": "이름"}, {"id": "u2", "text": "글"}, {"id": "ux1", "name": "이름", "text": "글"}):
        buttons.on_saved_item(event)
    assert buttons.custom == []


# --- 위치 조정 ---


def test_layout_from_board_is_echoed_merged(o):
    o.pending_pop()
    for row, text in enumerate(("esc,commit", "", ",".join(b for r in DEFAULT_ROWS for b in r if b not in ("esc", "commit")))):
        o.on_event({"orca": "saved_row", "row": row, "ids": text})
    o.on_event({"orca": "saved_end"})
    assert ids(o.pending_pop())[:2] == [["esc", "commit"], []]


# --- 미러 HTTP ---


@pytest.fixture
def server(o):
    view = mirror.Mirror({"enabled": False}, lambda: None)  # 4013 은 쓰지 않고 빈 포트로 띄운다
    view.orca = o
    http_server = mirror.ThreadingHTTPServer(("127.0.0.1", 0), view._handler())
    threading.Thread(target=http_server.serve_forever, daemon=True).start()
    yield http_server.server_address[1]
    http_server.shutdown()
    http_server.server_close()


def call(port, method, body=None, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    sent = {"Host": f"127.0.0.1:{mirror.PORT}", **(headers or {})}
    data = None
    if body is not None:
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        sent.setdefault("Content-Type", "application/json")
    conn.request(method, "/edit", data, sent)
    res = conn.getresponse()
    raw = res.read()
    conn.close()
    return res.status, json.loads(raw) if raw else None


def test_http_get_and_save(o, server):
    assert call(server, "GET") == (200, {})
    req = begin(o, "commit")
    assert call(server, "GET") == (200, req)
    code, reply = call(server, "POST", {"seq": req["seq"], "id": "commit", "name": "한글이름", "text": "한글 글 "})
    assert (code, reply) == (200, {})
    Board().take(o)
    assert o.buttons.names["commit"] == "한글이름" and o.buttons.texts["commit"] == "한글 글 "
    assert call(server, "GET") == (200, {})


def test_http_errors(o, server):
    req = begin(o, "commit")
    assert call(server, "POST", {"seq": req["seq"] + 1, "cancel": True})[0] == 409
    code, reply = call(server, "POST", {"seq": req["seq"], "id": "commit", "name": "", "text": "x"})
    assert code == 400 and reply["error"] == "표시명을 적어 주세요"
    assert call(server, "POST", b"{not json")[0] == 400
    assert call(server, "POST", {"seq": req["seq"], "cancel": True}) == (200, {})


@pytest.mark.parametrize("headers", [
    {"Origin": "https://evil.example"},
    {"Sec-Fetch-Site": "cross-site"},
    {"Sec-Fetch-Site": "same-site"},
    {"Host": "evil.example:4013"},
])
def test_http_other_pages_blocked(o, server, headers):
    req = begin(o, "commit")
    assert call(server, "GET", headers=headers)[0] == 404
    body = {"seq": req["seq"], "id": "commit", "name": "해킹", "text": "rm -rf ~"}
    assert call(server, "POST", body, headers=headers)[0] == 404
    assert o.buttons.texts == {}


def test_http_form_post_blocked(o, server):
    req = begin(o, "commit")
    body = json.dumps({"seq": req["seq"], "id": "commit", "name": "해킹", "text": "rm"}).encode()
    assert call(server, "POST", body, {"Content-Type": "text/plain"})[0] == 404
    assert o.buttons.texts == {}


def test_page_has_edit_dialog():
    assert 'id="editName"' in mirror.PAGE and "editPoll()" in mirror.PAGE


def test_open_editor_opens_only_without_app_window(monkeypatch):
    opened = []
    monkeypatch.setattr(mirror, "open_window", lambda browser: opened.append(browser) or "")
    monkeypatch.setattr(mirror.threading, "Thread",
                        lambda target, args, daemon: type("T", (), {"start": staticmethod(lambda: target(*args))}))
    view = mirror.Mirror({"enabled": False}, lambda: None)
    view.app_poll = mirror.time.time()
    view.open_editor()
    assert opened == []
    view.app_poll = 0
    view.open_editor()
    assert opened == [True]  # 앱도 페이지도 없다: 브라우저까지 연다
    view.last_poll = mirror.time.time()
    view.open_editor()
    assert opened == [True, False]  # 페이지가 있으면 앱 창만 연다 (숨은 탭일 수 있다)
