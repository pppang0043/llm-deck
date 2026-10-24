"""Windows 설치의 부품 풀기 (windows/unpack_wheels.py): 실행 파일·스크립트는 만들지 않는다."""

import importlib.util
import sys
import zipfile
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location("unpack_wheels", Path(__file__).parents[1] / "windows" / "unpack_wheels.py")
unpack_wheels = importlib.util.module_from_spec(spec)
spec.loader.exec_module(unpack_wheels)


def wheel(path: Path, files: dict[str, bytes]) -> Path:
    with zipfile.ZipFile(path, "w") as z:
        for name, data in files.items():
            z.writestr(name, data)
    return path


def test_unpacks_packages_without_scripts_or_exe(tmp_path, monkeypatch):
    wheels = tmp_path / "wheels"
    wheels.mkdir()
    wheel(wheels / "demo-1.0-cp312-cp312-win_amd64.whl", {
        "demo/__init__.py": b"x = 1",
        "demo/_native.pyd": b"\0",
        "demo/tool.exe": b"MZ",                                  # 부품 안에 든 exe
        "demo-1.0.dist-info/entry_points.txt": b"[console_scripts]\ndemo = demo:main\n",
        "demo-1.0.data/purelib/demo_extra.py": b"y = 2",
        "demo-1.0.data/scripts/demo-run.exe": b"MZ",             # 명령 실행 파일
        "demo-1.0.data/scripts/demo.bat": b"@echo off",
        "demo-1.0.data/headers/demo.h": b"",
    })
    target = tmp_path / "lib"
    monkeypatch.setattr(sys, "argv", ["unpack_wheels.py", str(wheels), str(target)])
    unpack_wheels.main()
    names = sorted(str(p.relative_to(target)).replace("\\", "/") for p in target.rglob("*") if p.is_file())
    assert names == ["demo-1.0.dist-info/entry_points.txt", "demo/__init__.py", "demo/_native.pyd", "demo_extra.py",
                     "python-version.txt"]


def test_refuses_names_outside_target(tmp_path):
    whl = wheel(tmp_path / "bad-1.0-py3-none-any.whl", {"../evil.py": b"", "bad/ok.py": b""})
    target = tmp_path / "lib"
    target.mkdir()
    skipped = unpack_wheels.unpack(whl, target)
    assert skipped == ["../evil.py"]
    assert not (tmp_path / "evil.py").exists()


def test_no_wheels_is_an_error(tmp_path, monkeypatch):
    monkeypatch.setattr(sys, "argv", ["unpack_wheels.py", str(tmp_path), str(tmp_path / "lib")])
    with pytest.raises(SystemExit):
        unpack_wheels.main()
