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
