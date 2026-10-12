"""기기와 통신하는 세 가지 방법 (USB / 블루투스 / Wi-Fi).

모두 같은 모양을 가진다:
    t = Transport(cfg, on_audio)   on_audio(seq, pcm) 은 마이크 소리 20ms 분량이 올 때마다 불린다
    t.open() -> bool               기기를 찾아 연결한다. 못 찾으면 False
    t.send(message: dict)          JSON 메시지를 보낸다. 실패하면 OSError
    t.close()
"""

from .ble import BleTransport
from .usb import UsbTransport
from .wifi import WifiTransport

# 자동 선택(auto)일 때 시도하는 순서
TRANSPORTS = {"usb": UsbTransport, "ble": BleTransport, "wifi": WifiTransport}
