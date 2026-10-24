"""proto 5: 제어 명령의 처리 완료 확인과 재부팅을 넘는 중복 방지.

PC 는 보내기 전에 ID/해시만 저장한다 (Wi-Fi 비밀번호 같은 본문은 저장하지 않는다).
기기는 실행 전 ID 를 영구 기록한다. 응답 유실은 같은 ID 로 조회/재전송하고,
실행 중 재부팅은 uncertain 으로 남겨 사람이 결과를 확인하게 한다.
"""

import hashlib
import json
import os
import re
import threading
import time
import uuid
from pathlib import Path

from .keepalive import KeepaliveTransport

COMMAND_FIELDS = frozenset({
    "mic", "battery_mv", "camera", "view", "camera_mount", "saver", "i2c_test", "usb_reconnect",
    "rotate", "photo_test", "wifi_add", "wifi_clear", "ble_use", "bt_check", "sd_pull",
    "health_clear", "hang_test", "crash_test", "sd_clear", "screenshot", "links", "camera_ctl",
})
STATES = frozenset({"queued", "running", "done", "failed", "uncertain", "unknown", "stale",
                    "rejected", "invalid", "forbidden", "clients_full", "busy", "conflict", "storage_error"})


class CommandError(OSError):
    """명령 오류. main 이 새 ID 로 자동 재시도하면 안 된다."""


class CommandUncertain(CommandError):
    pass


class CommandStore:
    def __init__(self, path=None):
        self.path = Path(path) if path else Path.home() / ".llm-deck" / "commands.json"
        self.lock = threading.Lock()
        try:
            self.data = json.loads(self.path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            self.data = {"client": uuid.uuid4().hex, "next": 1, "pending": {}}
        except (OSError, ValueError) as exc:
            raise CommandError("명령 기록을 읽지 못했습니다. commands.json 을 확인하세요 (자동 초기화하지 않습니다).") from exc
        data = self.data
        if (not isinstance(data, dict) or not re.fullmatch(r"[0-9a-f]{32}", str(data.get("client", "")))
                or type(data.get("next")) is not int or not 1 <= data["next"] <= 0x100000000
                or not isinstance(data.get("pending"), dict)):
            raise CommandError("명령 기록 형식이 잘못되었습니다. 자동 초기화하지 않습니다.")
        for device, pending in data["pending"].items():
            if (not re.fullmatch(r"[0-9a-fA-F]{12}", device) or not isinstance(pending, dict)
                    or type(pending.get("seq")) is not int or not 0 < pending["seq"] < data["next"]
                    or not re.fullmatch(r"[0-9a-f]{64}", str(pending.get("digest", "")))):
                raise CommandError("미완료 명령 기록이 잘못되었습니다. 자동 초기화하지 않습니다.")

    def _save(self):
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        tmp = self.path.with_name(self.path.name + "." + uuid.uuid4().hex + ".tmp")
        try:
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as out:
                json.dump(self.data, out)
                out.flush()
                os.fsync(out.fileno())
            os.replace(tmp, self.path)
            if hasattr(os, "O_DIRECTORY"):
                directory = os.open(self.path.parent, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
        finally:
            tmp.unlink(missing_ok=True)

    def pending(self, device):
        with self.lock:
            record = self.data["pending"].get(device)
            return dict(record) if record else None

    def begin(self, device, digest):
        with self.lock:
            if device in self.data["pending"]:
                raise CommandUncertain("앞 명령의 결과 확인이 필요합니다. --command-status 로 확인하세요.")
            seq = self.data["next"]
            if seq > 0xFFFFFFFF:
                raise CommandError("명령 순번을 모두 사용했습니다. 자동으로 ID 를 초기화하지 않습니다.")
            self.data["next"] += 1
            self.data["pending"][device] = {"seq": seq, "digest": digest}
            self._save()  # 저장하지 못하면 아직 기기에 보내지 않는다
            return self.data["client"], seq

    def finish(self, device):
        with self.lock:
            pending = self.data["pending"].pop(device, None)
            try:
                self._save()
            except BaseException:
                # rename/fsync 중 어느 단계에서 실패해도 재조회할 ID 를 메모리에 보존한다.
                if pending is not None:
                    self.data["pending"][device] = pending
                raise


_store = None
_store_lock = threading.Lock()


def default_store():
    global _store
    with _store_lock:
        if _store is None:
            _store = CommandStore()
        return _store


class CommandTransport:
    def __init__(self, raw, store=None, *, ack_timeout=1.5, command_timeout=20, poll_interval=0.2):
        self._store = store
        self._condition = threading.Condition()
        self._responses = {}
        self._command_lock = threading.Lock()
        self._ack_timeout = ack_timeout
        self._command_timeout = command_timeout
        self._poll_interval = poll_interval
        self._device = None
        # USB 는 pair 키를 먼저 처리한 뒤 _on_event 로 넘긴다.
        callback = "_on_event" if hasattr(raw, "_on_event") else "on_event"
        self._original_event = getattr(raw, callback)
        setattr(raw, callback, self._on_event)
        self._transport = KeepaliveTransport(raw)

    def __getattr__(self, name):
        return getattr(self._transport, name)

    @property
    def on_confirm(self):
        return self._transport.on_confirm

    @on_confirm.setter
    def on_confirm(self, value):
        self._transport.on_confirm = value

    def _on_event(self, event):
        key = None
        if isinstance(event, dict):
            if event.get("command_proto") == 1 and re.fullmatch(r"[0-9a-fA-F]{12}", str(event.get("id", ""))):
                key = "protocol"
            ack = event.get("ack")
            if (isinstance(ack, list) and len(ack) == 3 and isinstance(ack[0], str)
                    and type(ack[1]) is int and isinstance(ack[2], str) and ack[2] in STATES):
                key = (ack[0], ack[1])
        if key is not None:
            with self._condition:
                self._responses[key] = event
                while len(self._responses) > 32:
                    self._responses.pop(next(iter(self._responses)))
                self._condition.notify_all()
        else:
            self._original_event(event)

    def _exchange(self, message, key):
        with self._condition:
            self._responses.pop(key, None)
        self._transport.send(message)
        with self._condition:
            if not self._condition.wait_for(lambda: key in self._responses, self._ack_timeout):
                raise TimeoutError("명령 응답 시간 초과")
            return self._responses.pop(key)

    def _probe(self):
        # 화면/사진 전송 중 짧은 응답이 늦거나 유실될 수 있다. 조회만 재시도한다.
        for attempt in range(3):
            try:
                event = self._exchange({"command_protocol": True}, "protocol")
                break
            except OSError as exc:
                if attempt == 2:
                    raise CommandError("명령 확인 응답을 받지 못했습니다. 연결 상태와 proto 5 이상 펌웨어를 확인하세요.") from exc
                time.sleep(self._poll_interval)
        self._device = event["id"].lower()
        self._store = self._store or default_store()

    def _query(self, client, seq):
        return self._exchange({"command_status": {"client": client, "seq": seq}}, (client, seq))["ack"][2]

    def command_status(self, acknowledge=False):
        with self._command_lock, self._transport.command_wait():
            self._probe()
            pending = self._store.pending(self._device)
            if not pending:
                return {"device": self._device, "state": "none"}
            state = self._query(self._store.data["client"], pending["seq"])
            if acknowledge:
                if state not in {"done", "failed", "uncertain", "unknown", "stale"}:
                    raise CommandError(f"아직 기록을 해제할 수 없습니다: {state}")
                self._store.finish(self._device)
            return {"device": self._device, "seq": pending["seq"], "state": state, "acknowledged": acknowledge}

    def send(self, message):
        control = {k: v for k, v in message.items() if k in COMMAND_FIELDS}
        if not control:
            return self._transport.send(message)
        with self._command_lock, self._transport.command_wait():
            self._probe()
            payload = json.dumps(control, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)
            digest = hashlib.sha256(payload.encode()).hexdigest()
            previous = self._store.pending(self._device)
            if previous:
                try:
                    state = self._query(self._store.data["client"], previous["seq"])
                except OSError as exc:
                    raise CommandUncertain("앞 명령의 상태를 조회하지 못했습니다. 자동 재실행하지 않습니다.") from exc
                if state not in {"done", "failed"}:
                    raise CommandUncertain(f"앞 명령은 {state} 상태입니다. --command-status 로 확인한 뒤 --acknowledge-command 로 기록을 해제하세요.")
                try:
                    self._store.finish(self._device)
                except OSError as exc:
                    raise CommandUncertain("앞 명령의 PC 기록을 갱신하지 못했습니다.") from exc
                if previous["digest"] == digest:
                    # ACK 직후 PC 가 꺼져 같은 CLI 를 다시 실행한 경우도 새 ID 로 실행하지 않는다.
                    if state == "done":
                        print(f"[device] 앞 명령 {previous['seq']}의 완료를 확인했습니다 (재실행하지 않음).")
                        return
                    raise CommandError("앞 명령의 실패 결과를 확인했습니다 (재실행하지 않음).")
            normal = {k: v for k, v in message.items() if k not in COMMAND_FIELDS}
            if normal:
                self._transport.send(normal)
            try:
                client, seq = self._store.begin(self._device, digest)
            except OSError as exc:
                raise CommandError(f"명령 기록 저장 실패: {exc}") from exc
            envelope = {"command": {"client": client, "seq": seq, "json": payload}}
            timeout = 180 if "bt_check" in control or "sd_pull" in control else self._command_timeout
            deadline = time.monotonic() + timeout
            attempts, state = 0, "unknown"
            while time.monotonic() < deadline:
                try:
                    if state == "unknown" and attempts < 3:
                        attempts += 1
                        state = self._exchange(envelope, (client, seq))["ack"][2]
                    else:
                        state = self._query(client, seq)
                except OSError:
                    # 전송/ACK 유실 뒤에는 먼저 조회한다. 다음 unknown 응답에만 같은 ID 로 재전송.
                    state = "query"
                    time.sleep(self._poll_interval)
                    continue
                if state in {"done", "failed", "invalid", "forbidden", "clients_full", "rejected", "busy"}:
                    try:
                        self._store.finish(self._device)
                    except OSError as exc:
                        raise CommandUncertain(f"명령은 {state} 상태이나 PC 기록 갱신 실패: {exc}") from exc
                    if state == "done":
                        return
                    raise CommandError(f"기기 명령 처리 결과: {state}")
                if state in {"uncertain", "stale", "conflict", "storage_error"}:
                    break
                time.sleep(self._poll_interval)
            raise CommandUncertain(f"명령 {seq}의 결과를 확정하지 못했습니다 ({state}). 자동 재실행하지 않습니다. --command-status 로 확인하세요.")
