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


def test_silent_device_reported(monkeypatch):
    # 펌웨어 올리기가 끊겨 기기가 멈추면 창이 마지막 화면 그대로 먹통처럼 보였다. 오래 아무것도 안 오면 알린다
    class Usb:
        name = "USB"
        last_rx = 0.0

    usb = Usb()
    view = mirror.Mirror({"enabled": False}, lambda: usb)
    now = [1000.0]
    monkeypatch.setattr(mirror.time, "time", lambda: now[0])
    view.last_poll = now[0]
    assert view.pending() == {"mirror": True}
    assert not view.silent()  # 막 켰다
    now[0] += mirror.SILENT_SECONDS + 1
    assert view.silent()
    usb.last_rx = now[0] - 10  # 기기가 다른 것(기록)은 보내고 있다
    assert not view.silent()
    usb.last_rx = 0.0
    view.alive_at = now[0]  # 화면이 왔다
    assert not view.silent()


def test_split_state_and_button(monkeypatch):
    # 미러링 앱의 합치기·분리 단추: 기기가 알린 상태를 X-Split 으로 넘기고, 누르면 {"split": bool} 을 보낸다
    import http.client
    import json
    import threading

    class Usb:
        name = "USB"
        last_rx = 0.0
        sent = []

        def send(self, message):
            self.sent.append(message)

    usb = Usb()
    view = mirror.Mirror({"enabled": False}, lambda: usb)
    server = mirror.ThreadingHTTPServer(("127.0.0.1", 0), view._handler())
    threading.Thread(target=server.serve_forever, daemon=True).start()

    def call(method, path, body=None):
        conn = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=5)
        headers = {"Host": f"127.0.0.1:{mirror.PORT}", "Content-Type": "application/json"}
        conn.request(method, path, json.dumps(body) if body is not None else None, headers)
        res = conn.getresponse()
        res.read()
        return res.status, res.getheader("X-Split")

    try:
        monkeypatch.setattr(view.cond, "wait_for", lambda *a, **k: None)
        assert call("GET", "/frame?after=0") == (204, None)  # 옛 펌웨어: 단추를 숨긴다
        view.on_event({"mirror_split": True})
        view.on_event({"mirror_split": "x"})  # 잘못된 값은 무시
        assert call("GET", "/frame?after=0") == (204, "1")
        assert call("POST", "/split", {"on": False})[0] == 204
        assert usb.sent == [{"split": False}]
        assert call("POST", "/split", {"on": 1})[0] == 400
        assert call("POST", "/split", {})[0] == 400
        assert len(usb.sent) == 1
        view.on_event({"mirror_split": False})
        assert call("GET", "/frame?after=0") == (204, "0")
        view.link_lost()  # 끊기면 모른다
        assert call("GET", "/frame?after=0") == (204, None)
    finally:
        server.shutdown()
