"""Windows 의 COM 부품을 ctypes 로 부른다 (pywin32 같은 따로 까는 부품, PowerShell 없이).

답 읽어 주기(speech.py, SAPI)와 설치(windows/install.py, 시작 메뉴 바로 가기)가 쓴다.
COM 부품은 함수 목록(vtable)의 몇 번째 함수인지로 부른다. 번호는 Windows SDK 헤더에 적힌 순서다.
"""

from __future__ import annotations

import ctypes
from ctypes import POINTER, Structure, byref, c_ubyte, c_ulong, c_ushort, c_void_p, c_wchar_p

CLSCTX_ALL = 23


class GUID(Structure):
    _fields_ = [("d1", c_ulong), ("d2", c_ushort), ("d3", c_ushort), ("d4", c_ubyte * 8)]


def guid(text: str) -> GUID:
    g = GUID()
    ctypes.windll.ole32.CLSIDFromString(c_wchar_p(text), byref(g))
    return g


def init(apartment: bool = False) -> None:
    """이 스레드에서 COM 을 쓴다 (이미 했으면 그대로). 실패하면 OSError"""
    hr = ctypes.windll.ole32.CoInitializeEx(None, 2 if apartment else 0)  # STA / MTA. 1 = 이미 했다
    # 0x80010106 = 이 스레드는 이미 다른 방식으로 했다. 그래도 쓸 수 있다
    if hr not in (0, 1) and hr & 0xFFFFFFFF != 0x80010106:
        raise OSError(f"COM 을 쓸 수 없습니다 (0x{hr & 0xFFFFFFFF:08X})")


def create(clsid: str, iid: str) -> c_void_p:
    """부품을 만든다. 실패하면 OSError (HRESULT)"""
    ole32 = ctypes.windll.ole32
    ole32.CoCreateInstance.restype = ctypes.HRESULT
    obj = c_void_p()
    ole32.CoCreateInstance(byref(guid(clsid)), None, CLSCTX_ALL, byref(guid(iid)), byref(obj))
    return obj


def call(obj: c_void_p, index: int, argtypes: tuple = (), *args) -> None:
    """COM 함수 (vtable 의 index 번째). 실패하면 OSError (HRESULT)"""
    vtable = ctypes.cast(obj, POINTER(POINTER(c_void_p))).contents
    ctypes.WINFUNCTYPE(ctypes.HRESULT, c_void_p, *argtypes)(vtable[index])(obj, *args)


def query(obj: c_void_p, iid: str) -> c_void_p:
    """같은 부품의 다른 함수 묶음 (IUnknown::QueryInterface)"""
    other = c_void_p()
    call(obj, 0, (POINTER(GUID), POINTER(c_void_p)), byref(guid(iid)), byref(other))
    return other


def release(obj: c_void_p) -> None:
    if obj:
        vtable = ctypes.cast(obj, POINTER(POINTER(c_void_p))).contents
        ctypes.WINFUNCTYPE(c_ulong, c_void_p)(vtable[2])(obj)
