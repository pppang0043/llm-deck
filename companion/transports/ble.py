"""블루투스(BLE)로 통신한다. 형식은 펌웨어의 ble_link.h 참고."""

import asyncio
import json
import struct
import threading

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from . import adpcm

DEVICE_NAME = "LLM-Deck"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # PC -> 기기
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # 기기 -> PC
AUDIO_HEADER = struct.Struct("<HhB")
SCAN_SECONDS = 8


class BleTransport:
    name = "Bluetooth"

    def __init__(self, cfg: dict, on_audio):
        self.on_audio = on_audio
        self.client = None
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
            return self._run(self._connect(), SCAN_SECONDS + 20)
        except TimeoutError:
            # macOS 는 블루투스 권한이 없으면 오류 없이 계속 기다리기만 한다
            print("[Bluetooth] 응답이 없습니다. Mac 이라면 시스템 설정 > 개인정보 보호 및 보안 > Bluetooth 에서 터미널 앱을 허용했는지 확인하세요.")
            return False
        except (BleakError, OSError) as e:
            print(f"[Bluetooth] 연결 실패: {e}")
            return False

    async def _connect(self) -> bool:
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_SECONDS)
        if device is None:
            return False
        client = BleakClient(device)
        await client.connect()
        await client.start_notify(NUS_TX, self._on_notify)
        self.client = client
        return True

    def _on_notify(self, _sender, data: bytearray) -> None:
        seq, predictor, index = AUDIO_HEADER.unpack_from(data)
        self.on_audio(seq, adpcm.decode(bytes(data[AUDIO_HEADER.size :]), predictor, index))

    def send(self, message: dict) -> None:
        try:
            self._run(self._write(json.dumps(message, separators=(",", ":")).encode() + b"\n"), 30)
        except (BleakError, TimeoutError, AttributeError) as e:
            raise OSError(str(e) or type(e).__name__) from e

    async def _write(self, data: bytes) -> None:
        # 한 번에 쓸 수 있는 크기가 작아서 여러 조각으로 나눠 보낸다
        chunk = self.client.mtu_size - 3
        for i in range(0, len(data), chunk):
            await self.client.write_gatt_char(NUS_RX, data[i : i + chunk], response=True)

    def close(self) -> None:
        if self.client:
            try:
                self._run(self.client.disconnect(), 10)
            except (BleakError, OSError, TimeoutError):
                pass
            self.client = None
