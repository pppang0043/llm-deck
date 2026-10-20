"""기기와 통신하는 세 가지 방법 (USB / Wi-Fi / 블루투스).

모두 같은 모양을 가진다:
    t = Transport(cfg, on_audio, on_event, on_photo, on_video)
                                   on_audio(seq, pcm) 은 마이크 소리 20ms 분량이 올 때마다 불린다
                                   on_event(dict) 는 기기가 알림을 보낼 때 불린다 (예: {"sound": "mic_on"})
                                   on_photo(offset, data) 는 사진 조각이 올 때 불린다 (photos.py)
                                   on_video(no, offset, total, ms, data) 는 동영상 조각이 올 때 (videos.py)
    t.open() -> bool               기기를 찾아 연결한다. 못 찾으면 False
    t.send(message: dict)          JSON 메시지를 보낸다. 실패하면 OSError
    t.close()
"""

from .ble import BleTransport
from .usb import UsbTransport
from .wifi import WifiTransport

# 자동 선택(auto)일 때 시도하는 순서. 기기의 우선순위(link.cpp)와 같다. 블루투스는 Wi-Fi 를 쓸 수 없는 곳(회사 WIPS 등)을 위한 마지막 방법
TRANSPORTS = {"usb": UsbTransport, "wifi": WifiTransport, "ble": BleTransport}
