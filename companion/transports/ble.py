"""블루투스(BLE)로 통신한다. 형식은 펌웨어의 ble_link.h 참고.

BLE 는 암호화 없이 붙으므로, Wi-Fi 와 같은 키(USB 로 한 번 받아 간 것, auth.py)로 서로 확인하고 서명한다.
붙자마자 도전값을 보내 진짜 기기인지 확인하고, 그 뒤로 보내는 줄과 받는 알림마다 서명을 붙이고 확인한다.
"""

import asyncio
import concurrent.futures
import json
import os
import struct
import sys
import threading
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from . import adpcm, auth

DEVICE_NAME = "LLM-Deck"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # PC -> 기기
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # 기기 -> PC
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
AUDIO_HEADER = struct.Struct("<HhB")
AUDIO_SIZE = AUDIO_HEADER.size + 160  # 20ms = 320샘플 x 4비트
# 찾으면 바로 끝나므로 넉넉히 둔다. Windows 는 블루투스 장치가 많이 붙어 있으면 광고를 자주 놓친다.
SCAN_SECONDS = 20
# 한 번 연결한 기기의 주소. Windows 는 주소만 알면 찾기(스캔) 없이 바로 연결할 수 있다.
ADDRESS_FILE = Path.home() / ".llm-deck" / "ble_address"
DIRECT_SECONDS = 10
# 기기가 도전값에 답하기를 기다리는 시간
HELLO_SECONDS = 5
NOTICE_GAP = 600
_noticed: dict[str, float] = {}


def _notice(text: str) -> None:
    """같은 안내는 10분에 한 번만 (다시 연결할 때마다 쌓이지 않게)."""
    now = time.monotonic()
    if now - _noticed.get(text, -NOTICE_GAP) >= NOTICE_GAP:
        _noticed[text] = now
        print(text)


def _is_deck(device, adv) -> bool:
    return (adv.local_name == DEVICE_NAME or device.name == DEVICE_NAME
            or NUS_SERVICE in (u.lower() for u in adv.service_uuids))


class BleTransport:
    name = "Bluetooth"

    def __init__(self, cfg: dict, on_audio, on_event=None, on_photo=None, on_video=None):
        self.on_audio = on_audio
        self.on_event = on_event or (lambda event: None)
        self.on_photo = on_photo or (lambda offset, data: None)
        self.client = None
        # 인증 (ble_link.h). key 가 None 이면 아직 확인하지 못한 연결이다
        self.key = None
        self.pc_nonce = b""
        self.dev_nonce = b""
        self.ctr = 0
        self.rx_last = 0
        self._hello = None
        # bleak 은 asyncio 방식이라, 전용 스레드에서 이벤트 루프를 돌리고 일을 맡긴다
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()

    def _run(self, coro, timeout: float):
        future = asyncio.run_coroutine_threadsafe(coro, self.loop)
        try:
            return future.result(timeout)
        except TimeoutError:
            future.cancel()
            raise

    def open(self) -> bool:
        try:
            return self._run(self._connect(), DIRECT_SECONDS + SCAN_SECONDS + 20)
        except TimeoutError:
            # macOS 는 블루투스 권한이 없으면 오류 없이 계속 기다리기만 한다
            print("[Bluetooth] 응답이 없습니다. Mac 이라면 시스템 설정 > 개인정보 보호 및 보안 > Bluetooth 에서 터미널 앱을 허용했는지 확인하세요.")
            return False
        except (BleakError, OSError) as e:
            print(f"[Bluetooth] 연결 실패: {e}")
            return False
        except (asyncio.CancelledError, concurrent.futures.CancelledError):
            # Windows 는 연결이 늦으면 bleak 이 안에서 취소하고 끝낸다. 다음 차례에 다시 시도한다.
            print("[Bluetooth] 연결이 중간에 취소되었습니다. 다시 시도합니다.")
            return False

    async def _connect(self) -> bool:
        client = await self._connect_known() if sys.platform == "win32" else None
        if client is None:
            # 기기가 이미 다른 PC 와 연결되어 있으면 광고에 이름이 빠지기도 한다. 서비스 번호나 운영체제가 기억한 이름으로도 찾는다
            device = await BleakScanner.find_device_by_filter(_is_deck, timeout=SCAN_SECONDS)
            if device is None:
                return False
            client = BleakClient(device)
            await client.connect()
        await client.start_notify(NUS_TX, self._on_notify)
        self.client = client
        if not await self._authenticate():
            self.client = None
            try:
                await client.disconnect()
            except (BleakError, OSError):
                pass
            return False
        if sys.platform == "win32":
            try:
                ADDRESS_FILE.parent.mkdir(parents=True, exist_ok=True)
                ADDRESS_FILE.write_text(client.address)
            except OSError:
                pass
        return True

    async def _connect_known(self):
        """지난번에 연결한 주소로 바로 붙어 본다. 안 되면 None (찾기로 넘어간다)."""
        try:
            address = ADDRESS_FILE.read_text().strip()
        except OSError:
            return None
        if not address:
            return None
        client = BleakClient(address, timeout=DIRECT_SECONDS)
        try:
            await client.connect()
        except (BleakError, OSError, asyncio.TimeoutError):
            return None
        # 같은 주소의 다른 기기일 수 있으니 LLM Deck 서비스가 있는지 확인한다
        if not any(s.uuid.lower() == NUS_SERVICE for s in client.services):
            await client.disconnect()
            return None
        print("[Bluetooth] 지난번 주소로 바로 연결했습니다.")
        return client

    async def _authenticate(self) -> bool:
        """도전값을 보내고 기기의 서명을 확인한다. 맞으면 이후 주고받을 서명 준비를 마치고 True."""
        self.key = None
        self.pc_nonce = os.urandom(16)
        self._hello = self.loop.create_future()
        await self._write(json.dumps({"ble_hello": self.pc_nonce.hex()}, separators=(",", ":")).encode() + b"\n")
        try:
            reply = await asyncio.wait_for(self._hello, HELLO_SECONDS)
        except asyncio.TimeoutError:
            _notice("[Bluetooth] 기기가 블루투스 인증에 답하지 않습니다. 기기 펌웨어를 업데이트하세요.")
            return False
        if not isinstance(reply, dict):
            return False
        if reply.get("err") == "unpaired":
            _notice(f"[Bluetooth] 기기에 아직 인증 키가 없습니다. {auth.NEED_USB.replace('Wi-Fi', '블루투스')}")
            return False
        device_id = str(reply.get("id", ""))
        key = auth.load_key(device_id)
        if not key:
            _notice(f"[Bluetooth] 이 기기({device_id})의 인증 키가 없습니다. {auth.NEED_USB.replace('Wi-Fi', '블루투스')}")
            return False
        try:
            dev_nonce, tag = bytes.fromhex(reply.get("n", "")), bytes.fromhex(reply.get("t", ""))
        except (ValueError, TypeError):
            return False
        if len(dev_nonce) != 16 or not auth.ble_hello_ok(key, self.pc_nonce, dev_nonce, tag):
            # 키가 없는 가짜 기기일 수 있다. 붙지 않는다 (가짜 기기가 이 PC 에 입력·앱 실행을 시킬 수 있다)
            _notice("[Bluetooth] 서명이 맞지 않는 기기라 연결하지 않습니다. 기기 키가 바뀌었다면 USB 로 한 번 연결하세요.")
            return False
        self.key, self.dev_nonce, self.ctr, self.rx_last = key, dev_nonce, 0, 0
        return True

    def _on_notify(self, _sender, data: bytearray) -> None:
        # 인증하기 전에는 기기의 답(ble_auth)만 받는다
        if self.key is None:
            if data[:1] == b"{" and self._hello is not None:
                try:
                    reply = json.loads(bytes(data)).get("ble_auth")
                except (ValueError, AttributeError):
                    return
                if reply is not None:
                    self.loop.call_soon_threadsafe(lambda: self._hello.done() or self._hello.set_result(reply))
            return
        # 알림은 JSON 글자. 소리 묶음은 길이가 정해져 있어서(165바이트) 길이로 구별한다
        if len(data) != AUDIO_SIZE and data[:1] == b"\xff":
            # 사진 조각: [0xFF][위치 3바이트][데이터]
            self.on_photo(int.from_bytes(data[1:4], "little"), bytes(data[4:]))
            return
        if data[:1] == b"{" and len(data) != AUDIO_SIZE:
            # 서명이 맞고 순번이 앞으로 간 것만 따른다 (알림은 이 PC 에 입력·앱 실행을 시킨다)
            opened = auth.ble_event_open(self.key, self.pc_nonce, bytes(data))
            if not opened or opened[0] <= self.rx_last:
                _notice("[Bluetooth] 서명이 맞지 않는 알림을 버렸습니다.")
                return
            self.rx_last = opened[0]
            try:
                self.on_event(json.loads(opened[1]))
            except ValueError:
                pass
            return
        seq, predictor, index = AUDIO_HEADER.unpack_from(data)
        self.on_audio(seq, adpcm.decode(bytes(data[AUDIO_HEADER.size :]), predictor, index))

    def connected(self) -> bool:
        return bool(self.client and self.client.is_connected)

    def send(self, message: dict) -> None:
        try:
            self.ctr += 1
            line = auth.ble_line(self.key, self.dev_nonce, self.ctr, json.dumps(message, separators=(",", ":")).encode())
            self._run(self._write(line), 30)
        except (BleakError, TimeoutError, AttributeError, TypeError) as e:
            raise OSError(str(e) or type(e).__name__) from e

    async def _write(self, data: bytes) -> None:
        # 한 번에 쓸 수 있는 크기가 작아서 여러 조각으로 나눠 보낸다
        chunk = self.client.mtu_size - 3
        for i in range(0, len(data), chunk):
            for attempt in range(10):
                try:
                    await self.client.write_gatt_char(NUS_RX, data[i : i + chunk], response=True)
                    break
                except BleakError as e:
                    # 기기가 사진을 보내는 중이면 받을 자리가 잠깐 없어 "Insufficient Resource" 로 거절한다.
                    # 연결을 끊지 말고 기다렸다 다시 쓴다.
                    busy = "Insufficient Resource" in str(e) or "INSUFFICIENT_RESOURCE" in str(e)
                    if not busy or attempt == 9:
                        raise
                    await asyncio.sleep(0.5)

    def close(self) -> None:
        if self.client:
            try:
                self._run(self.client.disconnect(), 10)
            except (BleakError, OSError, TimeoutError):
                pass
            self.client = None
        self.key = None
