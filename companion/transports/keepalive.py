"""사용량 수집/음성 처리와 별개로 연결을 유지한다 (proto 4).

기기는 신호가 15초 끊기면 다음 연결을 고른다. 옛 펌웨어도 JSON 을 무시하면서
기존 연결 시각을 갱신하므로 같은 신호를 보낼 수 있다.
"""

import threading
from contextlib import contextmanager


class KeepaliveTransport:
    def __init__(self, transport, interval=5.0):
        self._transport = transport
        self._interval = interval
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._error = None
        self._command_wait = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True, name="deck-heartbeat")
        self._thread.start()

    def __getattr__(self, name):
        return getattr(self._transport, name)

    @property
    def on_confirm(self):
        return self._transport.on_confirm

    @on_confirm.setter
    def on_confirm(self, value):
        self._transport.on_confirm = value

    def send(self, message):
        # BLE 는 여러 조각으로 쓰므로 생존 신호가 중간에 끼어들면 안 된다.
        with self._lock:
            if self._stop.is_set() or self._error is not None:
                raise OSError(self._error or "연결이 닫혔습니다")
            self._transport.send(message)

    def _run(self):
        while not self._stop.wait(self._interval):
            try:
                self.send({"heartbeat": True})
            except Exception as exc:
                # 진단/촬영 루틴이 펌웨어 메인 루프를 오래 쓰면 HTTP 응답도 늦어진다.
                # 명령 대기 중에는 조회 자체의 한계시간에 맡기고 생존 확인은 계속 시도한다.
                if self._command_wait.is_set() and not self._stop.is_set():
                    continue
                if not self._stop.is_set():
                    self._error = f"생존 신호 전송 실패: {exc}"
                    print(f"[device] {self._error}")
                return

    @contextmanager
    def command_wait(self):
        self._command_wait.set()
        try:
            yield
        finally:
            self._command_wait.clear()

    def connected(self):
        return not self._stop.is_set() and self._error is None and self._transport.connected()

    def close(self):
        self._stop.set()
        # 쓰기가 진행 중이면 먼저 끝낸다. 닫힌 BLE 루프에 작업을 예약하지 않게 한다.
        with self._lock:
            self._transport.close()
        self._thread.join(timeout=1)
