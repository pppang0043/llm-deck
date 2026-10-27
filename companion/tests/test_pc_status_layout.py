"""기기 PC 상태의 고정 레이아웃이 가로·세로·미러링에서 화면 안에 들어가는지 검증."""
import shutil
import subprocess
from pathlib import Path

import pytest


def test_native_pc_status_layout(tmp_path):
    firmware = Path(__file__).resolve().parents[2] / "firmware"
    compiler = shutil.which("c++")
    if not compiler:
        pytest.skip("C++ compiler required")
    binary = tmp_path / "pc-layout"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(firmware / "src"),
                    str(firmware / "tests/pc_status_layout_test.cpp"), "-o", str(binary)],
                   check=True, capture_output=True, timeout=30)
    subprocess.run([str(binary)], check=True, capture_output=True, timeout=10)
