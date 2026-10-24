"""LLM Deck PC 도우미 설치 (Windows). PowerShell 없이 이 PC 에 깔린 Python 으로 한다.

    install.cmd    설치하고 로그인할 때 자동으로 켜지게 한다 (다시 실행해도 된다)
    uninstall.cmd  자동 실행과 Claude Code 상태줄 설정을 되돌린다 (install.py --uninstall)

install.cmd 가 아무 Python 이나 찾아 이 파일을 돌리고, 여기서 도우미를 돌릴 Python(3.11~3.14, Python 재단이 서명한
python.exe·pythonw.exe)을 고른다. 새 exe 를 만들지 않는다:
  - 가상 환경(.venv)을 만들지 않는다 (python.exe 복사본과 pip 가 만드는 명령 exe 가 생긴다)
  - pip 로는 부품 파일(wheel)을 받기만 하고 (pip download, 파일 지문 확인) unpack_wheels.py 가 app\\lib 에 푼다
  - uv 는 쓰지 않는다 (회사 보안이 막는다)
예전에는 install.ps1 을 실행 정책을 우회해 돌렸다. 회사 보안 프로그램이 의심하는 모양이고
PowerShell 을 막아 둔 PC 도 있어 Python 으로 옮겼다. 레지스트리는 winreg, 바로 가기·프로세스는 ctypes 로 Windows 를 바로 부른다.
부품 목록은 requirements.txt (버전과 파일 지문을 고정해 둔 것. Mac 에서 make_zip.py 가 uv.lock 으로 만든다)

install.cmd 가 찾은 Python 이 오래된 것(3.8~)일 수도 있어 이 파일은 옛 문법으로 적는다.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent  # app\windows
DIR = HERE.parent  # app
sys.path.insert(1, str(DIR))  # wincom

RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
NAME = "LLMDeck"
LOG_FILE = Path.home() / ".llm-deck" / "companion.log"
# 도우미 부품 (unpack_wheels.py 가 푼다. main.py·app.pyw 가 스스로 import 경로에 넣는다)
LIB = DIR / "lib"
WHEELS = DIR / ".wheels"
# 설치 때 고른 Python (mirror.cmd·uninstall.cmd 가 읽는다)
PYTHON_FILE = DIR / "python.txt"
# 예전 설치의 가상 환경 (python.exe 복사본과 pip 가 만든 exe 가 들어 있다). 보이면 지운다
VENV = DIR / ".venv"
# 도우미 앱: 트레이 아이콘을 두고 도우미를 띄우며, 거기서 미러링 창을 연다 (Python 재단이 서명한 pythonw.exe 로 돌린다)
APP_PYW = HERE / "app.pyw"
APP_ICON = HERE / "llm-deck.ico"
PROGRAMS = Path(os.environ.get("APPDATA", str(Path.home()))) / "Microsoft" / "Windows" / "Start Menu" / "Programs"
APP_LINK = PROGRAMS / "LLM Deck.lnk"
MIN_PYTHON, MAX_PYTHON = (3, 11), (3, 14)
# 예전에 쓰던 것들 (설치 때 컴파일한 exe, uv 설정, PowerShell 설치 파일). 보이면 지운다
OLD_FILES = (HERE / "LLM Deck.exe", HERE / "LLM Deck Mirror.exe", PROGRAMS / "LLM Deck 미러링.lnk", DIR / "uv.toml",
             HERE / "install.ps1")
OLD_HELPER_KEY = r"Software\LLMDeck\Helper"
# 터미널의 orca 명령 (orca.cmd). 부를 때마다 ORCA 가 깔린 곳을 찾아, ORCA 를 다시 깔거나 옮겨도 된다
SHIM_DIR = Path.home() / ".local" / "bin"
ORCA_SHIM = SHIM_DIR / "orca.cmd"
SHIM_MARK = "LLM Deck orca shim"
# Wi-Fi 로 연결했을 때 기기가 PC 로 보내는 것(알림·사진 알림·마이크 소리는 UDP 4010, 동영상은 TCP 4011)
FIREWALL_RULES = (("LLM Deck UDP 4010", "UDP", 4010), ("LLM Deck TCP 4011", "TCP", 4011))

_NO_WINDOW = 0x08000000  # CREATE_NO_WINDOW


class Fail(Exception):
    """설치를 멈춘다. 글은 사용자에게 그대로 보인다"""


def say(text: str) -> None:
    print(text, flush=True)


# ---- config.toml ----

def read_section(path: Path, section: str) -> dict:
    """config.toml 의 한 칸([storage] 등) 값. 오래된 Python 에는 tomllib 가 없어 줄 단위로 간단히 읽는다"""
    values: dict = {}
    if not path.exists():
        return values
    inside = False
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        t = line.strip()
        m = re.match(r"^\[([^\]]+)\]", t)
        if m:
            inside = m.group(1).strip() == section
            continue
        if not inside:
            continue
        m = re.match(r"^(\w+)\s*=\s*'([^']*)'", t)
        if m:
            values[m.group(1)] = m.group(2)
            continue
        m = re.match(r'^(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"', t)
        if m:
            values[m.group(1)] = m.group(2).replace("\\\\", "\\")
            continue
        m = re.match(r"^(\w+)\s*=\s*(true|false)\b", t)
        if m:
            values[m.group(1)] = m.group(2) == "true"
    return values


def storage_path(values: dict, name: str) -> str | None:
    """비어 있으면 None. %USERPROFILE% 같은 것은 풀어 준다"""
    v = values.get(name)
    if not isinstance(v, str) or not v.strip():
        return None
    path = os.path.expandvars(v.strip()).rstrip("\\")
    if path.endswith(":"):
        path += "\\"  # D:\ 처럼 드라이브만 적었을 때
    # 없는 드라이브(빼 둔 USB 등)나 C:\ 로 시작하지 않는 경로면 무엇이 틀렸는지 알려 주고 멈춘다
    drive = os.path.splitdrive(path)[0]
    if not drive or not os.path.exists(drive + "\\"):
        raise Fail(f"config.toml [storage] 의 {name} 를 쓸 수 없습니다: {path}  "
                   "(D:\\폴더 처럼 드라이브부터 적고, 그 드라이브가 있는지 확인하세요)")
    return path


def add_missing_sections(config: Path, example: Path) -> None:
    """예전 설정에는 저장 위치·회사망 칸이 없다. 예시 파일 끝의 그 부분을 그대로 붙여 둔다"""
    text = config.read_text(encoding="utf-8-sig")
    sample = example.read_text(encoding="utf-8-sig")
    marks = (("storage", "# 저장 위치:"), ("network", "# 회사망:"))
    for section, mark in marks:
        if re.search(rf"^\s*\[{section}\]", text, re.M):
            continue
        at = sample.find(mark)
        if at < 0:
            continue
        # 다음 칸의 설명이 나오기 전까지
        end = len(sample)
        for _, other in marks:
            nxt = sample.find(other, at + 1)
            if at < nxt < end:
                end = nxt
        block = "\r\n" + sample[at:end].rstrip().replace("\r\n", "\n").replace("\n", "\r\n") + "\r\n"
        with open(config, "a", encoding="utf-8", newline="") as f:
            f.write(block)
        text += block
        say(f"      [{section}] 칸을 config.toml 끝에 더했습니다. (INSTALL.txt 참고)")


def use_network(values: dict, env: dict) -> None:
    """회사망 설정([network])을 이번 설치 동안만 적용한다. pip 는 이 환경 변수들을 읽는다"""
    proxy = values.get("proxy")
    if isinstance(proxy, str) and proxy.strip():
        proxy = proxy.strip()
        if not re.match(r"^https?://", proxy):
            raise Fail(f"config.toml [network] 의 proxy 는 http:// 로 시작해야 합니다: {proxy}")
        env.update(HTTPS_PROXY=proxy, HTTP_PROXY=proxy, NO_PROXY="localhost,127.0.0.1")
        say(f"      회사망: 프록시 {proxy}")
    index = values.get("index_url")
    if isinstance(index, str) and index.strip():
        if not re.match(r"^https?://", index.strip()):
            raise Fail(f"config.toml [network] 의 index_url 은 https:// 로 시작해야 합니다: {index}")
        env["PIP_INDEX_URL"] = index.strip()
        say(f"      회사망: 부품 저장소 {env['PIP_INDEX_URL']}")
    if values.get("system_certs") is True:
        # 보안 프로그램이 연결을 검사하는 회사: Windows 에 깔린 (회사가 넣은) 인증서를 파일로 꺼내 pip 에 준다
        import ssl

        pem = DIR / ".windows-certs.pem"
        seen = set()
        out = []
        for store in ("ROOT", "CA"):
            for der, encoding, _trust in ssl.enum_certificates(store):
                if encoding == "x509_asn" and der not in seen:
                    seen.add(der)
                    out.append(ssl.DER_cert_to_PEM_cert(der))
        pem.write_text("".join(out), encoding="ascii")
        env["PIP_CERT"] = str(pem)
        say("      회사망: Windows 에 깔린 회사 인증서를 믿습니다")


# ---- Python 고르기 ----

def run_quiet(args: list, env: dict | None = None, cwd: Path | None = None) -> tuple:
    """프로그램을 돌려 (성공했는지, 출력 줄들). 오류 출력은 버린다"""
    # Python 이 경로(한글 사용자 이름 등)를 UTF-8 로 내보내게 한다 (그대로 두면 이 PC 의 글자 코드 cp949)
    env = dict(env if env is not None else os.environ, PYTHONIOENCODING="utf-8")
    try:
        r = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env,
                           cwd=cwd, timeout=600, creationflags=_NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        return False, []
    try:
        out = r.stdout.decode("utf-8")
    except UnicodeDecodeError:  # Python 이 아닌 프로그램(py.exe, netsh)은 이 PC 의 글자 코드로 내보낸다
        out = r.stdout.decode("mbcs" if sys.platform == "win32" else "utf-8", "replace")
    return r.returncode == 0, out.splitlines()


def python_info(exe: str) -> dict | None:
    """Python 하나를 돌려 버전을 본다. 64비트 Python 만 쓴다 (부품이 64비트용이다)"""
    if not exe or not os.path.isfile(exe):
        return None
    ok, out = run_quiet([exe, "-c", "import sys; print('%d.%d.%d' % sys.version_info[:3]); "
                                    "print(sys.maxsize > 2**32); print(sys.executable)"])
    if not ok or len(out) < 3:
        return None
    try:
        version = tuple(int(x) for x in out[0].strip().split("."))
    except ValueError:
        return None
    return {"version": version, "bits64": out[1].strip() == "True", "exe": out[2].strip()}


def registry_pythons() -> list:
    """python.org 설치 프로그램이 적어 두는 곳"""
    import winreg

    found = []
    places = ((winreg.HKEY_CURRENT_USER, r"Software\Python\PythonCore", 0),
              (winreg.HKEY_LOCAL_MACHINE, r"Software\Python\PythonCore", winreg.KEY_WOW64_64KEY),
              (winreg.HKEY_LOCAL_MACHINE, r"Software\Python\PythonCore", winreg.KEY_WOW64_32KEY))
    for root, path, view in places:
        try:
            core = winreg.OpenKey(root, path, 0, winreg.KEY_READ | view)
        except OSError:
            continue
        with core:
            i = 0
            while True:
                try:
                    tag = winreg.EnumKey(core, i)
                except OSError:
                    break
                i += 1
                try:
                    with winreg.OpenKey(core, tag + r"\InstallPath") as k:
                        try:
                            found.append(winreg.QueryValueEx(k, "ExecutablePath")[0])
                        except OSError:
                            found.append(os.path.join(winreg.QueryValueEx(k, "")[0], "python.exe"))
                except OSError:
                    continue
    return found


def python_candidates(storage: dict) -> list:
    """이 PC 에 깔린 Python 을 모두 찾는다 (앞에 있을수록 먼저 고른다)"""
    found = []
    # config.toml [storage] python 에 적은 것 (python.exe 나 그 폴더)
    for key in ("python", "python_dir"):
        v = storage_path(storage, key)
        if v:
            found.append(v if os.path.isfile(v) else os.path.join(v, "python.exe"))
    # Python 실행기(py.exe)가 아는 것
    py = shutil.which("py")
    if py:
        for line in run_quiet([py, "-0p"])[1]:
            m = re.search(r"([A-Za-z]:\\.*?\.exe)\s*$", line)
            if m:
                found.append(m.group(1))
    # PATH 에 있는 것 (Microsoft Store 의 바로 가기는 Python 이 없으면 실행되지 않아 걸러진다)
    for folder in os.environ.get("PATH", "").split(os.pathsep):
        if folder and os.path.isfile(os.path.join(folder, "python.exe")):
            found.append(os.path.join(folder, "python.exe"))
    found += registry_pythons()
    for base in (os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "Python"), os.environ.get("ProgramFiles", "")):
        if base and os.path.isdir(base):
            for d in sorted(Path(base).glob("Python3*")):
                found.append(str(d / "python.exe"))
    # 지금 이 파일을 돌리는 Python
    found.append(sys.executable)
    return found


def pick_python(infos: list) -> tuple:
    """3.11~3.14 중 가장 새 것과, 쓰지 않은 것들의 이유"""
    best = None
    others = []
    seen = set()
    for info in infos:
        key = info["exe"].lower()
        if key in seen:
            continue
        seen.add(key)
        v = info["version"]
        if not info["bits64"] or not (MIN_PYTHON <= v[:2] <= MAX_PYTHON):
            others.append("%s%s %s" % (".".join(map(str, v)), "" if info["bits64"] else " (32비트)", info["exe"]))
            continue
        # uv 가 받아 둔 Python 은 서명되지 않은 exe 라 회사 보안 프로그램이 막는다. 설치 프로그램으로 깐 것만 쓴다
        if "\\uv\\" in key:
            others.append("%s (uv 가 받은 것, 쓰지 않음) %s" % (".".join(map(str, v)), info["exe"]))
            continue
        # 창 없이 돌릴 pythonw.exe 가 옆에 있어야 한다
        if not os.path.isfile(os.path.join(os.path.dirname(info["exe"]), "pythonw.exe")):
            others.append("%s (pythonw.exe 없음) %s" % (".".join(map(str, v)), info["exe"]))
            continue
        if best is None or v > best["version"]:
            best = info
    return best, others


def select_python(storage: dict) -> dict:
    infos = []
    checked = set()
    for exe in python_candidates(storage):
        if exe.lower() in checked:
            continue
        checked.add(exe.lower())
        info = python_info(exe)
        if info:
            infos.append(info)
    best, others = pick_python(infos)
    if best:
        return best
    say("      쓸 수 있는 Python 을 찾지 못했습니다. (64비트 Python 3.11 ~ 3.14 가 필요합니다)")
    for o in others:
        say(f"      맞지 않는 버전: {o}")
    say('      Python 을 설치한 뒤 install.cmd 를 다시 실행하세요. (INSTALL.txt 의 "1. 준비")')
    say("        - 회사 PC: 회사 소프트웨어 센터(포털)에서 Python 3.12 또는 3.13 설치")
    say('        - 개인 PC: https://www.python.org/downloads/windows/ 의 "Windows installer (64-bit)"')
    say('                   설치 화면의 "tcl/tk" 와 "py launcher" 는 켜 둡니다 (기본값)')
    say("        - 깔려 있는데 못 찾으면 config.toml [storage] 에 python = 'C:\\경로\\python.exe' 로 적습니다")
    raise Fail("Python 이 필요합니다.")


# ---- 켜져 있는 도우미 끄기 ----

def processes() -> list:
    """(pid, 실행 파일 이름, 명령 줄) 목록. 명령 줄을 못 읽는 것은 빈 글자"""
    import ctypes
    from ctypes import wintypes

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", wintypes.LONG), ("dwFlags", wintypes.DWORD),
                    ("szExeFile", wintypes.WCHAR * 260)]

    class UNICODE_STRING(ctypes.Structure):
        _fields_ = [("Length", wintypes.USHORT), ("MaximumLength", wintypes.USHORT), ("Buffer", ctypes.c_void_p)]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    ntdll = ctypes.WinDLL("ntdll")
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    ntdll.NtQueryInformationProcess.restype = ctypes.c_long
    ntdll.NtQueryInformationProcess.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.ULONG,
                                                ctypes.POINTER(wintypes.ULONG)]

    def command_line(pid: int) -> str:
        h = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return ""
        try:
            size = wintypes.ULONG(0)
            ntdll.NtQueryInformationProcess(h, 60, None, 0, ctypes.byref(size))  # ProcessCommandLineInformation
            if not size.value:
                return ""
            buf = ctypes.create_string_buffer(size.value)
            if ntdll.NtQueryInformationProcess(h, 60, ctypes.cast(buf, ctypes.c_void_p), size, ctypes.byref(size)) != 0:
                return ""
            us = UNICODE_STRING.from_buffer(buf)
            return ctypes.wstring_at(us.Buffer, us.Length // 2) if us.Buffer else ""
        finally:
            kernel32.CloseHandle(h)

    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if not snap or snap == wintypes.HANDLE(-1).value:
        return []
    result = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            name = entry.szExeFile
            pid = entry.th32ProcessID
            cmd = command_line(pid) if name.lower() in ("python.exe", "pythonw.exe") else ""
            result.append((pid, name, cmd))
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return result


def ours(name: str, cmd: str) -> bool:
    """이 폴더의 도우미 앱·도우미인지 (예전 exe 로 된 앱 포함)"""
    if name.lower() in ("llm deck.exe", "llm deck mirror.exe"):
        return True
    if name.lower() not in ("python.exe", "pythonw.exe") or not cmd:
        return False
    c = cmd.lower()
    if "app.pyw" in c and str(HERE).lower() in c:
        return True
    # 앱 없이 켠 도우미(예전 방식이나 앱을 쓸 수 없을 때)도 끈다. 이 폴더의 도우미만
    return "main.py" in c and (str(DIR).lower() in c or "llm-deck" in c) and "install.py" not in c


def stop_companion() -> None:
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    handles = []
    for pid, name, cmd in processes():
        if pid == os.getpid() or not ours(name, cmd):
            continue
        h = kernel32.OpenProcess(0x0001 | 0x00100000, False, pid)  # PROCESS_TERMINATE | SYNCHRONIZE
        if h:
            kernel32.TerminateProcess(h, 1)
            handles.append(h)
    # 다 꺼져야 부품 폴더(lib)를 지우거나 새로 쓸 수 있다
    for h in handles:
        kernel32.WaitForSingleObject(h, 5000)
        kernel32.CloseHandle(h)


# ---- 레지스트리·파일 ----

def delete_tree(root, path: str) -> None:
    """레지스트리 키를 아래 키까지 지운다 (없으면 그대로)"""
    import winreg

    try:
        with winreg.OpenKey(root, path) as k:
            subs = []
            i = 0
            while True:
                try:
                    subs.append(winreg.EnumKey(k, i))
                except OSError:
                    break
                i += 1
        for sub in subs:
            delete_tree(root, path + "\\" + sub)
        winreg.DeleteKey(root, path)
    except OSError:
        pass


def set_run(value: str | None, name: str = NAME) -> None:
    """로그인할 때 자동 실행 (None 이면 지운다)"""
    import winreg

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
        if value is None:
            try:
                winreg.DeleteValue(k, name)
            except OSError:
                pass
        else:
            winreg.SetValueEx(k, name, 0, winreg.REG_SZ, value)


def remove_old() -> None:
    """예전 설치가 남긴 것 (설치 때 컴파일한 exe, 가상 환경, uv 설정, PowerShell 설치 파일)"""
    import winreg

    for f in OLD_FILES:
        try:
            f.unlink()
        except OSError:
            pass
    delete_tree(winreg.HKEY_CURRENT_USER, OLD_HELPER_KEY)
    if VENV.exists():
        shutil.rmtree(VENV, ignore_errors=True)
        say("      예전 부품 폴더(.venv, 새로 만든 exe 가 들어 있던 곳)를 지웠습니다.")
    # 이 폴더에는 exe 를 두지 않는다 (도우미는 이 PC 에 설치된 Python 만 실행한다)
    exes = [p for p in DIR.rglob("*") if p.suffix.lower() == ".exe" and p.is_file()]
    for p in exes:
        try:
            p.unlink()
        except OSError:
            pass
    if exes:
        say(f"      남아 있던 exe {len(exes)}개를 지웠습니다.")


def installed_python() -> tuple:
    """설치 때 고른 Python (python.exe, pythonw.exe). 없거나 지워졌으면 (None, None)"""
    try:
        exe = PYTHON_FILE.read_text(encoding="utf-8-sig").strip()
    except OSError:
        return None, None
    pyw = os.path.join(os.path.dirname(exe), "pythonw.exe")
    if exe and os.path.isfile(exe) and os.path.isfile(pyw):
        return exe, pyw
    return None, None


def broadcast_environment() -> None:
    """환경 변수를 바꿨다고 Windows 에 알린다 (새로 여는 터미널부터 적용된다)"""
    import ctypes
    from ctypes import wintypes

    result = wintypes.DWORD()
    ctypes.windll.user32.SendMessageTimeoutW(0xFFFF, 0x001A, 0, "Environment", 0x0002, 5000, ctypes.byref(result))


def path_with(raw: str, folder: Path) -> str | None:
    """사용자 PATH 에 folder 를 더한 값. 이미 있으면 None"""
    parts = [p for p in raw.split(";") if p]
    want = str(folder).rstrip("\\").lower()
    if any(os.path.expandvars(p).rstrip("\\").lower() == want for p in parts):
        return None
    return ";".join(parts + [str(folder)])


def install_orca_shim() -> None:
    import winreg

    # 사용자가 따로 둔 orca.cmd 는 건드리지 않는다
    if ORCA_SHIM.exists() and SHIM_MARK not in ORCA_SHIM.read_text(encoding="utf-8", errors="replace"):
        say(f"      건너뜁니다: {ORCA_SHIM} 이 이미 있습니다.")
        return
    SHIM_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(HERE / "orca.cmd", ORCA_SHIM)
    # 사용자 PATH 에 더한다. %USERPROFILE% 처럼 적힌 칸이 풀리지 않게 레지스트리 값을 그대로 읽고 쓴다
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, "Environment") as k:
        try:
            raw, kind = winreg.QueryValueEx(k, "Path")
        except OSError:
            raw, kind = "", winreg.REG_EXPAND_SZ
        new = path_with(str(raw), SHIM_DIR)
        if new is not None:
            winreg.SetValueEx(k, "Path", 0, winreg.REG_EXPAND_SZ if "%" in new else kind, new)
            broadcast_environment()
            say(f"      PATH 에 {SHIM_DIR} 를 더했습니다 (새로 연 터미널부터).")
    say(f"      orca 명령: {ORCA_SHIM}")


def make_shortcut(link: Path, target: str, arguments: str, workdir: Path, icon: Path | None, description: str) -> None:
    """시작 메뉴 바로 가기(.lnk). Windows 의 ShellLink 부품을 바로 부른다. 실패하면 OSError"""
    from ctypes import c_int, c_wchar_p

    import wincom

    wincom.init(apartment=True)  # 바로 가기 부품은 STA 에서 쓴다
    shell_link = wincom.create("{00021401-0000-0000-C000-000000000046}", "{000214F9-0000-0000-C000-000000000046}")
    persist = None
    try:
        # IShellLinkW: SetDescription 7, SetWorkingDirectory 9, SetArguments 11, SetIconLocation 17, SetPath 20
        wincom.call(shell_link, 20, (c_wchar_p,), target)
        wincom.call(shell_link, 11, (c_wchar_p,), arguments)
        wincom.call(shell_link, 9, (c_wchar_p,), str(workdir))
        wincom.call(shell_link, 7, (c_wchar_p,), description)
        if icon and icon.exists():
            wincom.call(shell_link, 17, (c_wchar_p, c_int), str(icon), 0)
        persist = wincom.query(shell_link, "{0000010B-0000-0000-C000-000000000046}")  # IPersistFile
        wincom.call(persist, 6, (c_wchar_p, c_int), str(link), 1)  # Save
    finally:
        wincom.release(persist)
        wincom.release(shell_link)


# ---- 방화벽 ----

def firewall_rules() -> list:
    """이미 있는 LLM Deck 규칙 이름 (읽기는 관리자 권한이 필요 없다)"""
    netsh = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "netsh.exe")
    return [name for name, _, _ in FIREWALL_RULES
            if run_quiet([netsh, "advfirewall", "firewall", "show", "rule", f"name={name}"])[0]]


def firewall_script(existing: list, add: bool) -> str:
    """netsh -f 로 돌릴 명령들. 있는 규칙만 지운다 (없는 규칙을 지우면 오류라서)"""
    lines = [f'advfirewall firewall delete rule name="{name}"' for name in existing]
    if add:
        # 같은 공유기 안에서 오는 것만 허용한다
        lines += [f'advfirewall firewall add rule name="{name}" dir=in action=allow protocol={proto} '
                  f"localport={port} remoteip=localsubnet profile=any" for name, proto, port in FIREWALL_RULES]
    return "\r\n".join(lines) + "\r\n"


def set_firewall(add: bool) -> bool:
    """관리자 확인 창(UAC)을 한 번 띄워 Windows 에 들어 있는 netsh 로 규칙을 바꾼다 (PowerShell 을 쓰지 않는다)"""
    import ctypes
    from ctypes import wintypes

    class SHELLEXECUTEINFOW(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("fMask", wintypes.ULONG), ("hwnd", wintypes.HWND),
                    ("lpVerb", wintypes.LPCWSTR), ("lpFile", wintypes.LPCWSTR), ("lpParameters", wintypes.LPCWSTR),
                    ("lpDirectory", wintypes.LPCWSTR), ("nShow", ctypes.c_int), ("hInstApp", wintypes.HINSTANCE),
                    ("lpIDList", ctypes.c_void_p), ("lpClass", wintypes.LPCWSTR), ("hkeyClass", wintypes.HKEY),
                    ("dwHotKey", wintypes.DWORD), ("hIcon", wintypes.HANDLE), ("hProcess", wintypes.HANDLE)]

    script = firewall_script(firewall_rules(), add)
    fd, path = tempfile.mkstemp(prefix="llm-deck-firewall-", suffix=".txt")
    with os.fdopen(fd, "w", encoding="ascii") as f:
        f.write(script)
    netsh = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "netsh.exe")
    try:
        info = SHELLEXECUTEINFOW()
        info.cbSize = ctypes.sizeof(info)
        info.fMask = 0x40  # SEE_MASK_NOCLOSEPROCESS
        info.lpVerb = "open" if ctypes.windll.shell32.IsUserAnAdmin() else "runas"
        info.lpFile = netsh
        info.lpParameters = f'-f "{path}"'
        info.nShow = 0  # 창 없이
        shell32 = ctypes.WinDLL("shell32", use_last_error=True)
        shell32.ShellExecuteExW.argtypes = [ctypes.POINTER(SHELLEXECUTEINFOW)]
        if not shell32.ShellExecuteExW(ctypes.byref(info)) or not info.hProcess:
            return False  # [아니요]를 눌렀다
        kernel32 = ctypes.windll.kernel32
        kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.WaitForSingleObject(info.hProcess, 60000)
        kernel32.CloseHandle(info.hProcess)
        return True
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


# ---- 설치·제거 ----

def start_detached(args: list) -> None:
    """설치 창을 닫아도 계속 돌게 띄운다"""
    subprocess.Popen([str(a) for a in args], cwd=DIR, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, close_fds=True,
                     creationflags=0x00000008 | 0x00000200)  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP


def uninstall() -> None:
    import winreg

    stop_companion()
    remove_old()
    for f in (APP_LINK, APP_ICON):
        try:
            f.unlink()
        except OSError:
            pass
    if ORCA_SHIM.exists() and SHIM_MARK in ORCA_SHIM.read_text(encoding="utf-8", errors="replace"):
        ORCA_SHIM.unlink()
    delete_tree(winreg.HKEY_CURRENT_USER, r"Software\LLMDeck")  # 미러링 창의 항상 위·배율·위치
    set_run(None)
    # 규칙이 있을 때만 지운다 (관리자 확인 창이 뜬다)
    if firewall_rules():
        set_firewall(add=False)
    py, _ = installed_python()
    if py and LIB.exists() and (Path.home() / ".claude").exists():
        subprocess.run([py, str(DIR / "main.py"), "--uninstall-statusline"], cwd=DIR,
                       env=dict(os.environ, PYTHONPATH=str(LIB)))
    say("제거했습니다. 이 폴더는 지워도 됩니다.")


def install() -> None:
    env = dict(os.environ)

    say("[1/8] 설정 파일과 저장 위치")
    config = DIR / "config.toml"
    example = DIR / "config.example.toml"
    if not config.exists():
        shutil.copyfile(example, config)
        say("      config.example.toml 을 config.toml 로 복사했습니다.")
    else:
        say("      기존 config.toml 을 그대로 씁니다.")
        add_missing_sections(config, example)
    network = read_section(config, "network")
    use_network(network, env)
    # 용량이 적은 PC 에서 큰 것을 다른 드라이브에 둘 때
    storage = read_section(config, "storage")
    model_dir = storage_path(storage, "model_dir")
    cache_dir = storage_path(storage, "cache_dir") or storage_path(storage, "uv_cache_dir")  # 예전 이름
    if cache_dir:
        os.makedirs(cache_dir, exist_ok=True)
        env["PIP_CACHE_DIR"] = cache_dir
        say(f"      설치 부품 보관함: {cache_dir}")
    if model_dir:
        os.makedirs(model_dir, exist_ok=True)

    say("[2/8] Python 찾기")
    python = select_python(storage)
    py = python["exe"]
    pyw = os.path.join(os.path.dirname(py), "pythonw.exe")
    say("      Python %s  %s" % (".".join(map(str, python["version"])), py))

    say("[3/8] 도우미 부품 (처음에는 몇 분 걸릴 수 있습니다)")
    if not run_quiet([py, "-m", "pip", "--version"], env)[0]:
        say('      이 Python 에 pip 가 없습니다. Python 설치 프로그램을 다시 열어 [Modify] > "pip" 를 켜고 install.cmd 를 다시 실행하세요.')
        raise Fail("pip 가 필요합니다.")
    # 버전과 파일 지문이 고정된 목록대로, 미리 만들어진 파일(wheel)만 받는다 (이 PC 에서 컴파일하지 않는다).
    # pip install 은 부품마다 명령 exe 를 새로 만들어서 pip 로는 받기만 하고, 푸는 것은 unpack_wheels.py 가 한다 (exe 를 만들지 않는다)
    new_lib = DIR / "lib.new"
    shutil.rmtree(WHEELS, ignore_errors=True)
    shutil.rmtree(new_lib, ignore_errors=True)
    ok = subprocess.run([py, "-m", "pip", "download", "--disable-pip-version-check", "--no-input", "--quiet",
                         "--only-binary", ":all:", "--require-hashes", "-r", str(DIR / "requirements.txt"),
                         "-d", str(WHEELS)], env=env).returncode == 0
    if ok:
        ok = subprocess.run([py, str(HERE / "unpack_wheels.py"), str(WHEELS), str(new_lib)], env=env).returncode == 0
    shutil.rmtree(WHEELS, ignore_errors=True)
    if not ok:
        shutil.rmtree(new_lib, ignore_errors=True)
        raise Fail('도우미 준비에 실패했습니다. 위 오류 문구를 보고 INSTALL.txt 의 "9. 회사 PC" 를 확인하세요.')
    # 켜져 있는 도우미가 부품 파일을 쓰고 있으면 지우지 못한다
    stop_companion()
    remove_old()
    if LIB.exists():
        shutil.rmtree(LIB)
    new_lib.rename(LIB)
    PYTHON_FILE.write_text(py, encoding="utf-8")
    # 아래 확인들도 그 부품을 쓴다 (도우미는 main.py·app.pyw 가 스스로 lib 를 넣는다)
    env["PYTHONPATH"] = str(LIB)
    # 고친 config.toml 에 틀린 곳이 없는지 본다 (틀리면 도우미가 켜지지 않는다)
    ok, out = run_quiet([py, "-c", "import tomllib, pathlib, sys\n"
                                   "try: tomllib.loads(pathlib.Path(sys.argv[1]).read_bytes().decode('utf-8-sig'))\n"
                                   "except Exception as e: print(e); sys.exit(1)", config], env)
    if not ok:
        say(f"      config.toml 에 틀린 곳이 있습니다: {out[-1] if out else ''}")
        say("      Windows 경로는 작은따옴표로 적으세요. 예: model_dir = 'D:\\LLM Deck\\models'")
        raise Fail("config.toml 을 고친 뒤 다시 실행하세요.")
    # 음성 질문용 부품(faster-whisper)이 이 PC 에서 켜지는지 본다. 안 돼도 나머지 기능은 쓸 수 있다.
    if not run_quiet([py, "-c", "import ctranslate2, onnxruntime, av, faster_whisper"], env)[0]:
        say('      주의: 음성 질문 부품을 켜지 못했습니다. "Microsoft Visual C++ 재배포 가능 패키지(x64)"를 설치한 뒤 다시 실행하세요.')
        say("            https://aka.ms/vs/17/release/vc_redist.x64.exe  (음성 질문 말고는 그대로 쓸 수 있습니다)")
    model_root = model_dir or str(Path.home() / ".llm-deck" / "models")
    whisper = read_section(config, "assistant").get("whisper_model")
    if not isinstance(whisper, str) or not whisper:
        whisper = "small"
    # whisper_model 에 폴더 경로를 적었으면 그 폴더를 쓴다 (assistant.py)
    model = os.path.join(whisper, "model.bin") if re.search(r"[\\/:]", whisper) else \
        os.path.join(model_root, f"faster-whisper-{whisper}", "model.bin")
    if os.path.exists(model):
        say(f"      음성 인식 모델: 복사해 둔 폴더를 씁니다 ({os.path.dirname(model)})")
    else:
        where = model_dir or str(Path.home() / ".cache" / "huggingface")
        say(f"      음성 인식 모델({whisper}): 처음 말로 질문할 때 인터넷으로 받습니다 ({where}). 회사망에서 막히면 INSTALL.txt 의 5 참고")

    say("[4/8] Claude Code 한도 사용률 읽기")
    if (Path.home() / ".claude").exists():
        subprocess.run([py, str(DIR / "main.py"), "--install-statusline"], cwd=DIR, env=env)
    else:
        say("      이 PC 에는 Claude Code 가 없어 건너뜁니다. (다른 PC 값은 gist 로 받아 옵니다)")

    say("[5/8] Windows 방화벽 (Wi-Fi 연결용)")
    # 관리자 권한으로 방화벽을 바꾸는 일이라 회사 보안 프로그램이 막을 수 있다. config.toml 에서 켰을 때만 한다
    if network.get("firewall") is not True:
        say("      건너뜁니다 (config.toml [network] firewall = false). USB·블루투스는 상관없습니다.")
        say("      Wi-Fi 로 쓰는데 사진·동영상·알림이 안 오면 firewall = true 로 바꾸고 install.cmd 를 다시 실행하세요.")
    else:
        say("      관리자 확인 창이 뜨면 [예]")
        if set_firewall(add=True):
            say(f"      허용 규칙 {len(firewall_rules())}개를 두었습니다.")
        else:
            say("      건너뜁니다. USB·블루투스는 상관없고, Wi-Fi 로 쓸 때 사진·동영상·알림이 안 올 수 있습니다.")
        # 예전에 방화벽 창에서 [취소]를 누르면 python.exe 를 막는 규칙이 생긴다. 막는 규칙이 허용보다 앞선다.
        say("      그래도 안 오면: Windows 보안 > 방화벽 및 네트워크 보호 > 고급 설정 > 인바운드 규칙에서")
        say("                      python 을 막는 규칙(빨간 표시)이 있는지 보고 지우세요.")

    say("[6/8] 도우미 앱 (트레이 아이콘, 미러링 창)")
    stop_companion()
    remove_old()
    app_ok = False
    if not run_quiet([py, "-c", "import tkinter; tkinter.Tcl()"], env)[0]:
        say("      건너뜁니다: 이 Python 에 창을 그리는 부품(tcl/tk)이 없습니다.")
        say('      Python 설치 프로그램을 다시 열어 [Modify] > "tcl/tk and IDLE" 를 켜고 install.cmd 를 다시 실행하세요.')
        say("      그 전까지 도우미는 트레이 아이콘 없이 돌고, mirror.cmd 를 누르면 브라우저로 엽니다 (http://127.0.0.1:4013/).")
    elif not run_quiet([py, "-c", "import pystray, PIL"], env)[0]:
        say("      건너뜁니다: 트레이 아이콘 부품(pystray)을 켜지 못했습니다. 도우미는 트레이 아이콘 없이 돕니다.")
    else:
        app_ok = True
        run_quiet([py, APP_PYW, "--write-icon", APP_ICON], env)
        try:
            make_shortcut(APP_LINK, pyw, f'"{APP_PYW}"', DIR, APP_ICON,
                          "LLM Deck 도우미를 켜고 기기 화면(미러링)을 연다")
            say('      작업 표시줄 오른쪽 아래의 LLM Deck 아이콘이나 시작 메뉴의 "LLM Deck" 으로 미러링 창을 엽니다.')
        except OSError as e:
            # 바로 가기가 없어도 도우미와 트레이 아이콘은 그대로 쓴다
            say(f"      시작 메뉴 바로 가기는 만들지 못했습니다 ({e}). 트레이 아이콘이나 mirror.cmd 로 엽니다.")

    say("[7/8] 터미널의 orca 명령")
    install_orca_shim()

    say("[8/8] 로그인할 때 자동 실행")
    # 도우미를 끈 뒤에 비운다 (켜져 있으면 쓰는 중인 파일이 있어 지우지 못할 수 있다)
    if storage.get("clean_cache") is True:
        run_quiet([py, "-m", "pip", "cache", "purge"], env)
        say("      설치 부품 보관함을 비웠습니다 (clean_cache = true)")
    # 예전 방식(공유만 하는 도우미)이 등록되어 있으면 지운다. 이제 이 도우미가 공유도 함께 한다.
    set_run(None, "LLMDeckSync")
    LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
    if app_ok:
        # 앱이 도우미를 띄운다. 로그인할 때는 미러링 창 없이 트레이 아이콘만
        set_run(f'"{pyw}" "{APP_PYW}" --background')
        start_detached([pyw, APP_PYW, "--background"])
    else:
        main_py = DIR / "main.py"
        set_run(f'"{pyw}" "{main_py}" --log "{LOG_FILE}"')
        start_detached([pyw, main_py, "--log", LOG_FILE])

    say("")
    if app_ok:
        say("설치했습니다. 도우미가 실행 중입니다 (작업 표시줄 오른쪽 아래 LLM Deck 아이콘).")
    else:
        say("설치했습니다. 도우미가 창 없이 실행 중입니다.")
    say(f"기록: {LOG_FILE}")
    say("기기 연결: USB 케이블, 블루투스(LLM-Deck), 같은 Wi-Fi 중 되는 것으로 자동 연결합니다.")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--uninstall", action="store_true")
    args = parser.parse_args()
    if sys.platform != "win32":
        raise SystemExit("Windows 에서만 씁니다.")
    try:
        uninstall() if args.uninstall else install()
    except Fail as e:
        say("")
        say(f"멈췄습니다: {e}")
        sys.exit(1)
    except Exception as e:  # 예상하지 못한 오류도 무엇인지 보이게
        say("")
        say(f"멈췄습니다 (예상하지 못한 오류): {type(e).__name__}: {e}")
        say(f"이 문구를 알려 주세요. 설치 파일: {__file__}")
        sys.exit(1)


if __name__ == "__main__":
    main()
