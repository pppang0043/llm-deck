"""녹음과 처리 나누기 (assistant.py): 앞 녹음을 처리하는 동안에도 새로 녹음하고, 녹음한 순서대로 처리한다.
마이크·음성 인식·CLI 대신 가짜를 넣는다."""

import threading
import time

import pytest

import assistant


@pytest.fixture
def a(monkeypatch):
    a = assistant.Assistant({})
    a.release = threading.Event()  # 처리를 막아 두었다가 풀어 준다
    a.done: list[assistant._Take] = []

    def record(take):
        take.chunks.append("소리")
        a._set_rec(take, "listening")

    def answer(take):
        a._set("thinking", a="받아 적는 중", mode=take.mode)
        a.release.wait(5)
        a.done.append(take)
        a._set("done", q=f"질문 {len(a.done)}", a="답", mode=take.mode)

    monkeypatch.setattr(a, "_record", record)
    monkeypatch.setattr(a, "_answer", answer)
    monkeypatch.setattr(a, "_warm", lambda take: None)
    monkeypatch.setattr(a, "_watch_mics", lambda: None)  # 이 PC 의 마이크를 열어 보지 않게
    return a


def wait_for(cond, seconds=3.0):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.01)
    return False


def test_can_record_while_previous_is_processing(a):
    a.on_event({"ask": "start"})
    a.on_event({"ask": "stop"})
    assert wait_for(lambda: a.busy)
    a.on_event({"ask": "start", "mode": "dictate"})
    assert a.recording, "앞 녹음을 처리하는 중에도 새로 녹음해야 한다"
    a.on_event({"ask": "stop"})
    assert a.takes.qsize() == 1
    a.release.set()
    assert wait_for(lambda: len(a.done) == 2)
    assert [t.mode for t in a.done] == ["ask", "dictate"], "녹음한 순서대로 처리한다"


def test_recording_state_is_sent_first_then_processing_with_queue(a):
    a.on_event({"ask": "start"})
    a.on_event({"ask": "stop"})
    assert wait_for(lambda: a.busy)
    a.on_event({"ask": "start"})
    assert wait_for(lambda: a.rec_state and a.rec_state["state"] == "listening")
    # 녹음 중이면 앞 녹음의 처리 상태가 아니라 녹음 상태를 보낸다 (기기의 녹음 버튼이 풀리지 않게)
    assert a.pending()["state"] == "listening"
    a.on_event({"ask": "stop"})
    sent = a.pending()
    assert sent["state"] == "thinking" and sent["queued"] == 1
    a.release.set()


def test_finished_take_cannot_change_recording_state(a):
    a.on_event({"ask": "start"})
    old = a.take
    a.on_event({"ask": "stop"})
    a.on_event({"ask": "start"})
    assert wait_for(lambda: a.rec_state and a.rec_state["state"] == "listening")
    a._set_rec(old, "preparing")  # 끝난 녹음의 스레드가 늦게 부른다
    assert a.rec_state["state"] == "listening"
    a.release.set()


def test_queue_is_limited(a):
    a.on_event({"ask": "start"})
    a.on_event({"ask": "stop"})
    assert wait_for(lambda: a.busy)
    for _ in range(assistant.MAX_QUEUED_TAKES):
        a.on_event({"ask": "start"})
        a.on_event({"ask": "stop"})
    a.on_event({"ask": "start"})
    assert not a.recording
    assert a.pending_toast
    a.release.set()


def test_answer_spoken_after_recording_ends(a, monkeypatch):
    spoken = []
    monkeypatch.setattr(a.speaker, "speak", spoken.append)
    monkeypatch.setattr(a.speaker, "stop", lambda: None)
    a.spoken_later = None
    a.on_event({"ask": "start"})
    a.spoken_later = "앞 답"  # 녹음하는 동안 끝난 답
    a.on_event({"ask": "stop"})
    assert spoken == ["앞 답"]
    a.release.set()


def test_new_chat_does_not_block_receiver(a):
    # 답하는 중 (lock 을 쥐고 있다) 에 "새 대화" 를 눌러도 USB 받는 스레드가 멈추지 않는다
    a.session = type("Session", (), {"closed": False, "close": lambda self: setattr(self, "closed", True)})()
    session = a.session
    a.lock.acquire()
    try:
        receiver = threading.Thread(target=a.on_event, args=({"ask": "new"},), daemon=True)
        receiver.start()
        receiver.join(10)  # 부하가 커도 lock 을 쥔 동안 돌아온다
        assert not receiver.is_alive()
        assert not session.closed
    finally:
        a.lock.release()
    assert wait_for(lambda: session.closed and a.session is None)


@pytest.mark.parametrize("copied,note", [(True, True), (False, False)])
def test_failed_dictation_is_kept_in_clipboard(a, monkeypatch, copied, note):
    clipboard = []
    monkeypatch.setattr(assistant.orca, "_copy", lambda text: (clipboard.append(text), copied)[1])
    monkeypatch.setattr(assistant.typer, "_type", lambda text, enter: "손쉬운 사용 권한이 없습니다")
    a.orca = None
    a._finish_dictation(assistant._Take(True, False, False), "말한 글")
    assert clipboard == ["말한 글"]
    assert a.pending_toast.startswith("입력하지 못했습니다\n손쉬운 사용")
    assert ("클립보드" in a.pending_toast) is note


def test_dictation_exception_is_kept_in_clipboard(a, monkeypatch):
    clipboard = []
    monkeypatch.setattr(assistant.orca, "_copy", lambda text: (clipboard.append(text), True)[1])
    a.orca = type("Fake", (), {"commands": lambda self: [], "aliases": {},
                               "send_text": lambda self, target, text: 1 / 0})()
    take = assistant._Take(True, False, True)
    take.orca_target = "term_x"
    a._finish_dictation(take, "말한 글")
    assert clipboard == ["말한 글"] and "클립보드" in a.pending_toast


def test_successful_dictation_leaves_clipboard_alone(a, monkeypatch):
    clipboard = []
    monkeypatch.setattr(assistant.orca, "_copy", lambda text: clipboard.append(text))
    monkeypatch.setattr(assistant.typer, "_type", lambda text, enter: None)
    a.orca = None
    a._finish_dictation(assistant._Take(True, False, False), "말한 글")
    assert clipboard == [] and a.pending_toast == "입력했습니다: 말한 글"
