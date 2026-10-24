import pytest

import typer


@pytest.fixture
def typed(monkeypatch):
    calls = []
    monkeypatch.setattr(typer, "_type", lambda text, enter=True, tab=False: calls.append((text, tab)) or None)
    return calls


def test_push_keyword_present():
    assert "push" in typer.KEYWORDS
    assert typer.KEYWORDS["push"]


@pytest.mark.parametrize("name", sorted(typer.KEYWORDS))
def test_whitelisted_keywords_typed(typed, name):
    t = typer.Typer()
    t.on_event({"type": name})
    assert typed == [(typer.KEYWORDS[name], False)]
    assert t.pending_toast.startswith("입력했습니다")


@pytest.mark.parametrize("event", [
    {"type": "rm -rf /"}, {"type": "커밋해줘"}, {"type": ""}, {"type": 1}, {"type": None}, {},
    {"type": "PUSH"},
])
def test_other_names_not_typed(typed, event):
    typer.Typer().on_event(event)
    assert typed == []


def test_tab_enter_presses_keys_only(typed):
    t = typer.Typer()
    t.on_event({"type": "tab_enter"})
    assert typed == [("", True)]


def test_type_error_reported(monkeypatch):
    monkeypatch.setattr(typer, "_type", lambda *a, **k: "막혔습니다")
    t = typer.Typer()
    t.on_event({"type": "push"})
    assert "입력하지 못했습니다" in t.pending_toast


@pytest.fixture
def restore():
    yield
    typer.configure(None)


def test_defaults_keep_old_ids():
    typer.configure(None)
    assert list(typer.KEYWORDS) == ["commit", "deploy", "push", "error", "orchestration", "clear", "compact"]
    assert typer.KEYWORDS["commit"] == "커밋해줘"
    assert typer.ITEMS[0] == {"id": "commit", "name": "커밋", "obj": "\"커밋해줘\"를"}
    assert typer.ITEMS[3]["obj"] == "\"에러 확인\"을"
    assert typer.ITEMS[4]["obj"] == "\"/orchestration\"을"
    assert typer.ITEMS[5]["obj"] == "\"/clear\"를"
    assert typer.ITEMS[6]["obj"] == "\"/compact\"을"
    # 기기에는 글을 보내지 않는다
    assert all(set(item) == {"id", "name", "obj"} for item in typer.ITEMS)


def test_configured_list_replaces_whitelist(typed, restore):
    typer.configure({"items": [{"name": "테스트", "text": "테스트 돌려줘"}, {"name": "리뷰", "text": "/review"}]})
    assert typer.KEYWORDS == {"k0": "테스트 돌려줘", "k1": "/review"}
    assert [i["name"] for i in typer.ITEMS] == ["테스트", "리뷰"]
    assert typer.ITEMS[0]["obj"] == "\"테스트 돌려줘\"를"
    t = typer.Typer()
    t.on_event({"type": "k1"})
    t.on_event({"type": "commit"})  # 설정한 목록에 없는 예전 이름은 입력하지 않는다
    assert typed == [("/review", False)]


@pytest.mark.parametrize("items", [
    [{"name": "", "text": "a"}],
    [{"name": "a", "text": ""}],
    [{"name": "a", "text": "줄\n바꿈"}],
    [{"name": "a\tb", "text": "x"}],
    [{"name": "가" * 13, "text": "x"}],
    [{"name": "a", "text": "x" * 201}],
    [{"name": 1, "text": "x"}],
    ["커밋"],
])
def test_bad_items_skipped(items, restore):
    typer.configure({"items": items})
    assert list(typer.KEYWORDS) == [kid for kid, _, _ in typer.DEFAULT_KEYWORDS]  # 남은 것이 없으면 기본 목록


def test_limits_and_long_text(restore):
    items = [{"name": f"버튼{n}", "text": f"글{n}"} for n in range(9)]
    items.insert(1, {"name": "나쁜", "text": "\x1b[2J"})
    items.insert(2, {"name": "긴 글", "text": "가" * 200})
    typer.configure({"items": items})
    assert list(typer.KEYWORDS) == ["k0", "k1", "k2", "k3", "k4", "k5", "k6"]
    assert typer.KEYWORDS["k1"] == "가" * 200
    assert typer.ITEMS[1]["obj"] == "\"긴 글\"을"  # 글이 길면 알림에는 버튼 이름


@pytest.mark.parametrize("word, josa", [
    ("커밋해줘", "를"), ("에러 확인", "을"), ("배포", "를"), ("/orchestration", "을"), ("/review", "를"),
    ("v2", "를"), ("step 3", "을"), ("😀", "을(를)"),
])
def test_particle(word, josa):
    assert typer.particle(word) == josa


class _FakeLsappinfo:
    """lsappinfo / open 흉내. order 는 앞에서부터의 앱 (bundle id)."""

    def __init__(self, order):
        self.order, self.opened = list(order), []

    def __call__(self, args, **kw):
        import subprocess

        out = ""
        if args[:2] == ["lsappinfo", "visibleProcessList"]:
            out = " ".join(f'ASN:0x0-0x{i + 1:x}-"app{i}":' for i in range(len(self.order)))
        elif args[:2] == ["lsappinfo", "front"]:
            out = "ASN:0x0-0x1:"
        elif args[:2] == ["lsappinfo", "info"]:
            index = int(args[-1].rsplit("0x", 1)[1], 16) - 1
            out = f'"CFBundleIdentifier"="{self.order[index]}"'
        elif args[:2] == ["open", "-b"]:
            self.opened.append(args[2])
            self.order.remove(args[2])
            self.order.insert(0, args[2])
        return subprocess.CompletedProcess(args, 0, out, "")


def test_mirror_window_in_front_returns_to_previous_app(monkeypatch):
    fake = _FakeLsappinfo([typer.OWN_BUNDLE, "com.stablyai.orca", "com.apple.finder"])
    monkeypatch.setattr(typer.subprocess, "run", fake)
    typer._leave_own_window()
    assert fake.opened == ["com.stablyai.orca"]


def test_other_app_in_front_is_left_alone(monkeypatch):
    fake = _FakeLsappinfo(["com.stablyai.orca", typer.OWN_BUNDLE])
    monkeypatch.setattr(typer.subprocess, "run", fake)
    typer._leave_own_window()
    assert fake.opened == []
