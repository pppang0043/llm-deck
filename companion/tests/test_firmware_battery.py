"""기기 없이 실제 C++ 배터리 잔량 계산(firmware/src/battery_level.cpp)을 검증한다."""
import shutil
import subprocess
from pathlib import Path

import pytest


def test_native_battery_level(tmp_path):
    firmware = Path(__file__).resolve().parents[2] / "firmware"
    compiler = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if not compiler or not firmware.exists():
        pytest.skip("C++ 컴파일러와 펌웨어 소스가 필요합니다")
    binary = tmp_path / "battery-test"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(firmware / "src"),
                    str(firmware / "tests/battery_level_test.cpp"), str(firmware / "src/battery_level.cpp"),
                    "-o", str(binary)], check=True, capture_output=True, text=True, timeout=30)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
