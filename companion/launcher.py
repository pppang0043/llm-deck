"""기기의 단축 버튼({"launch": 이름})을 누르면 이 PC 에서 프로그램을 연다.

여기 적어 둔 이름만 연다 (기기나 다른 사람이 아무 명령이나 실행시키지 못하게).
결과는 기기 화면에 짧은 알림으로 보낸다 (pending_toast).
"""

import os
import shutil
import subprocess
import sys
import threading

# 이름 -> (기기에 보일 이름 + 조사, Windows 실행 이름, Mac 앱 이름)
APPS = {
    "excel": ("엑셀을", "excel", "Microsoft Excel"),
    "powerpoint": ("파워포인트를", "powerpnt", "Microsoft PowerPoint"),
    "orca": ("ORCA를", "Orca", "Orca"),
    # Mac 에는 파워쉘 대신 터미널을 연다 (관리자도 같다: Mac 은 sudo 로 그때그때 권한을 얻는다)
    "powershell": ("파워쉘을", "powershell", "Terminal"),
    "powershell_admin": ("관리자 파워쉘을", "powershell", "Terminal"),
}
# Mac 에서 다른 프로그램을 열 때 기기에 보일 이름
MAC_LABELS = {"powershell": "터미널을", "powershell_admin": "터미널을"}
# 관리자 권한으로 여는 것 (Windows 가 "허용하시겠습니까?" 를 묻는다. PC 에서 직접 눌러야 열린다)
ADMIN = {"powershell_admin"}
# Windows 에서 열지 못했을 때 대신 여는 것 (회사가 PowerShell 을 막아 둔 PC 는 명령 프롬프트로)
WIN_FALLBACK = {"powershell": ("cmd", "파워쉘 대신 명령 프롬프트를")}
# App Paths 에 등록하지 않는 프로그램이 사용자 폴더에 깔리는 자리 (Windows)
WIN_PATHS = {
    "Orca": [r"%LOCALAPPDATA%\Programs\orca\Orca.exe"],
    # 새 PowerShell(7) 이 깔려 있으면 그것을, 없으면 Windows 에 들어 있는 것을 연다
    "powershell": [r"%ProgramFiles%\PowerShell\7\pwsh.exe",
                   r"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"],
    "cmd": [r"%SystemRoot%\System32\cmd.exe"],
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
        if sys.platform != "win32" and mac_name is None:
            self.pending_toast = f"{label} 열 수 없습니다\nWindows 에서만 됩니다"
            return
        admin = name in ADMIN and sys.platform == "win32"
        if sys.platform == "darwin":
            label = MAC_LABELS.get(name, label)

        # 관리자 권한은 사용자가 PC 에서 "예" 를 누를 때까지 기다리므로, 기기와 주고받는 일을 막지 않게 따로 연다
        cancelled: dict = {}

        def run() -> None:
            nonlocal label
            ok = self._open(win_name, mac_name, admin, cancelled)
            fallback = WIN_FALLBACK.get(win_name) if sys.platform == "win32" else None
            # 관리자 확인 창에서 [아니요]를 누른 것이면 다시 묻지 않는다
            if not ok and fallback and not cancelled.get(win_name) and self._open(fallback[0], None, admin):
                ok, label = True, fallback[1]
            print(f"[launch] {label} {'열었습니다' if ok else '열지 못했습니다'}")
            if ok:
                self.pending_toast = f"{label} 열었습니다"
            else:
                self.pending_toast = f"{label} 열지 못했습니다\n" + (
                    "PC 에서 허용하지 않았습니다" if admin else "설치되어 있는지 확인하세요")

        threading.Thread(target=run, daemon=True).start()

    @staticmethod
    def _open(win_name: str, mac_name: str | None, admin: bool = False, cancelled: dict | None = None) -> bool:
        try:
            if sys.platform == "win32":
                path = _app_path(win_name)
                if not path:
                    return False
                # 관리자 권한은 "runas": Windows 가 사용자에게 허용을 묻는다 (거절하면 OSError)
                try:
                    os.startfile(path, "runas" if admin else "open")  # noqa: S606 (APPS 에 적은 프로그램만)
                except OSError as e:
                    if cancelled is not None and getattr(e, "winerror", None) == 1223:  # ERROR_CANCELLED
                        cancelled[win_name] = True
                    raise
                return True
            elif sys.platform == "darwin":
                result = subprocess.run(["open", "-a", mac_name], capture_output=True, timeout=10)
            else:
                return False
            return result.returncode == 0
        except (OSError, subprocess.TimeoutExpired):
            return False
