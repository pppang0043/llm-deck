"""Windows GPU 사용률. 외부 exe 없이 PDH 를 읽으며, 지원하지 않으면 None.

프로세스별 GPU Engine 카운터를 물리 엔진별로 합한 뒤 가장 바쁜 엔진을 쓴다.
https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/
https://learn.microsoft.com/windows/win32/api/pdh/nf-pdh-pdhgetformattedcounterarrayw
"""
import atexit
import ctypes as ct
import math
import re
import sys

_DWORD = ct.c_uint32
_HANDLE = ct.c_void_p
_MORE_DATA = 0x800007D2
_FMT_DOUBLE = 0x200
_ENGINE = re.compile(r"(?:^|_)luid_(.+)_phys_(\d+)_eng_(\d+)(?:_|$)")


class _ValueData(ct.Union):
    _fields_ = [("doubleValue", ct.c_double), ("largeValue", ct.c_int64)]


class _Value(ct.Structure):
    _fields_ = [("status", _DWORD), ("data", _ValueData)]


class _Item(ct.Structure):
    _fields_ = [("name", ct.c_wchar_p), ("value", _Value)]


def _busiest_engine(samples: list[tuple[str, float]]) -> int | None:
    engines: dict[tuple, float] = {}
    for name, value in samples:
        match = _ENGINE.search(name)
        if match and math.isfinite(value) and value >= 0:
            key = match.groups()  # LUID, physical GPU, engine; PID 는 합한다
            engines[key] = engines.get(key, 0.0) + value
    return min(100, round(max(engines.values()))) if engines else None


class WindowsGpu:
    def __init__(self) -> None:
        self.query = _HANDLE()
        self.counter = _HANDLE()
        self.pdh = None
        self.retry_at = 0.0
        if sys.platform == "win32":
            try:
                self.pdh = ct.WinDLL("pdh")
                signatures = {
                    "PdhOpenQueryW": ([ct.c_wchar_p, ct.c_size_t, ct.POINTER(_HANDLE)], _DWORD),
                    "PdhAddEnglishCounterW": ([_HANDLE, ct.c_wchar_p, ct.c_size_t, ct.POINTER(_HANDLE)], _DWORD),
                    "PdhCollectQueryData": ([_HANDLE], _DWORD),
                    "PdhGetFormattedCounterArrayW": ([_HANDLE, _DWORD, ct.POINTER(_DWORD),
                                                       ct.POINTER(_DWORD), ct.POINTER(_Item)], _DWORD),
                    "PdhCloseQuery": ([_HANDLE], _DWORD),
                }
                for name, (args, result) in signatures.items():
                    fn = getattr(self.pdh, name)
                    fn.argtypes, fn.restype = args, result
                atexit.register(self.close)
            except (OSError, AttributeError):
                self.pdh = None

    def close(self) -> None:
        if self.query.value and self.pdh is not None:
            self.pdh.PdhCloseQuery(self.query)
        self.query = _HANDLE()
        self.counter = _HANDLE()

    def percent(self, now: float) -> int | None:
        if self.pdh is None or now < self.retry_at:
            return None
        if not self.query.value:
            # GPU 카운터가 없는 PC 에서 매 3초 초기화를 반복하지 않는다.
            self.retry_at = now + 60
            if self.pdh.PdhOpenQueryW(None, 0, ct.byref(self.query)):
                self.close()
                return None
            if self.pdh.PdhAddEnglishCounterW(self.query, r"\GPU Engine(*)\Utilization Percentage",
                                             0, ct.byref(self.counter)):
                self.close()
                return None
            self.pdh.PdhCollectQueryData(self.query)  # 비율은 두 번의 표본이 있어야 한다
            self.retry_at = 0
            return None
        if self.pdh.PdhCollectQueryData(self.query):
            self.close()
            self.retry_at = now + 60
            return None
        # 인스턴스 수가 바뀌면 길이를 다시 물어본다.
        for _ in range(3):
            size, count = _DWORD(), _DWORD()
            status = self.pdh.PdhGetFormattedCounterArrayW(self.counter, _FMT_DOUBLE,
                                                          ct.byref(size), ct.byref(count), None)
            if status != _MORE_DATA or not size.value:
                return None
            buffer = ct.create_string_buffer(size.value)
            items = ct.cast(buffer, ct.POINTER(_Item))
            status = self.pdh.PdhGetFormattedCounterArrayW(self.counter, _FMT_DOUBLE,
                                                          ct.byref(size), ct.byref(count), items)
            if status == _MORE_DATA:
                continue
            if status:
                return None
            return _busiest_engine([(items[i].name or "", items[i].value.data.doubleValue)
                                    for i in range(count.value) if items[i].value.status in (0, 1)])
        return None
