"""상단 쓸기 판정, 수동 절전과 깨우기/작업 중 CPU 보호를 실제 펌웨어 정책으로 검증."""
import shutil
import subprocess
from pathlib import Path

import pytest


def test_native_quick_controls(tmp_path):
    firmware = Path(__file__).resolve().parents[2] / "firmware"
    compiler = shutil.which("c++")
    if not compiler:
        pytest.skip("C++ compiler required")
    binary = tmp_path / "quick-controls"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(firmware / "src"),
                    str(firmware / "tests/quick_controls_test.cpp"), "-o", str(binary)],
                   check=True, capture_output=True, timeout=30)
    subprocess.run([str(binary)], check=True, capture_output=True, timeout=10)
