"""기기의 단축 버튼({"launch": 이름})을 누르면 이 PC 에서 프로그램을 연다.

여기 적어 둔 이름만 연다 (기기나 다른 사람이 아무 명령이나 실행시키지 못하게).
결과는 기기 화면에 짧은 알림으로 보낸다 (pending_toast).
"""

import os
import shutil
import subprocess
import sys

# 이름 -> (기기에 보일 이름 + 조사, Windows 실행 이름, Mac 앱 이름)
APPS = {
    "excel": ("엑셀을", "excel", "Microsoft Excel"),
    "powerpoint": ("파워포인트를", "powerpnt", "Microsoft PowerPoint"),
    "orca": ("ORCA를", "Orca", "Orca"),
}
# App Paths 에 등록하지 않는 프로그램이 사용자 폴더에 깔리는 자리 (Windows)
WIN_PATHS = {
    "Orca": [r"%LOCALAPPDATA%\Programs\orca\Orca.exe"],
}


def _app_path(exe: str) -> str | None:
    """Windows 가 프로그램 설치 때 적어 두는 App Paths 에서 실행 파일 위치를 찾는다 (Office 는 여기에 등록된다)."""
    import winreg

    for candidate in WIN_PATHS.get(exe, []):
        path = os.path.expandvars(candidate)
        if os.path.exists(path):
            return path

    key = rf"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{exe}.exe"
    for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        try:
            with winreg.OpenKey(root, key) as k:
                path = winreg.QueryValue(k, None)
        except OSError:
            continue
        path = path.strip('"')
        if path and os.path.exists(path):
            return path
    # Microsoft Store 판 Office 는 App Paths 대신 WindowsApps 폴더의 실행 이름(별칭)으로 들어온다
    return shutil.which(exe)


class Launcher:
    def __init__(self) -> None:
        self.pending_toast: str | None = None

    def on_event(self, event: dict) -> None:
        name = event.get("launch")
        if not isinstance(name, str):
            return
        app = APPS.get(name)
        if not app:
            print(f"[launch] 모르는 단축 버튼: {name}")
            self.pending_toast = "PC 도우미가 모르는 버튼입니다\n도우미를 새로 깔아 주세요"
            return
        label, win_name, mac_name = app
        ok = self._open(win_name, mac_name)
        print(f"[launch] {label} {'열었습니다' if ok else '열지 못했습니다'}")
        self.pending_toast = f"{label} 열었습니다" if ok else f"{label} 열지 못했습니다\n설치되어 있는지 확인하세요"

    @staticmethod
    def _open(win_name: str, mac_name: str) -> bool:
        try:
            if sys.platform == "win32":
                path = _app_path(win_name)
                if not path:
                    return False
                os.startfile(path)  # noqa: S606 (APPS 에 적은 프로그램만)
                return True
            elif sys.platform == "darwin":
                result = subprocess.run(["open", "-a", mac_name], capture_output=True, timeout=10)
            else:
                return False
            return result.returncode == 0
        except (OSError, subprocess.TimeoutExpired):
            return False
