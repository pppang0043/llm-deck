import mirror


def make(monkeypatch):
    opened = []
    monkeypatch.setattr(mirror, "open_window", lambda browser: opened.append(1) or "")
    # 창 열기를 따로 띄우지 않고 바로 해서 바로 셀 수 있게
    monkeypatch.setattr(mirror.threading, "Thread", lambda target, daemon: type("T", (), {"start": staticmethod(target)}))
    view = mirror.Mirror({"enabled": False}, lambda: None)
    view.auto_open = True
    return view, opened


def test_opens_on_first_connect_but_not_over_bluetooth(monkeypatch):
    view, opened = make(monkeypatch)
    view.link_up("Bluetooth")
    assert opened == []
    view.link_lost()
    view.link_up("Wi-Fi")  # 블루투스에서는 화면을 못 받았으니 연다
    view.link_up("Wi-Fi")
    assert len(opened) == 1


def test_quick_reconnect_stays_quiet_but_cable_opens(monkeypatch):
    view, opened = make(monkeypatch)
    view.link_up("Wi-Fi")
    view.link_lost()
    view.link_up("Wi-Fi")
    assert len(opened) == 1
    view.link_lost()
    view.link_up("USB")
    assert len(opened) == 2


def test_long_gap_and_open_window(monkeypatch):
    view, opened = make(monkeypatch)
    view.link_up("USB")
    view.link_lost()
    view.lost_at -= mirror.REJOIN_SECONDS
    view.app_poll = mirror.time.time()  # 창이 이미 떠 있다
    view.link_up("USB")
    assert len(opened) == 1
    view.app_poll = 0
    view.link_lost()
    view.lost_at -= mirror.REJOIN_SECONDS
    view.link_up("USB")
    assert len(opened) == 2
