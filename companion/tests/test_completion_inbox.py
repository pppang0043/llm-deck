import shutil
import subprocess
from pathlib import Path

import pytest


def test_shared_completion_inbox(tmp_path):
    compiler = shutil.which("c++")
    if not compiler:
        pytest.skip("C++ compiler required")
    firmware = Path(__file__).resolve().parents[2] / "firmware"
    binary = tmp_path / "completion-inbox"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(firmware / "src"),
                    str(firmware / "tests/completion_inbox_test.cpp"), "-o", str(binary)],
                   check=True, capture_output=True, timeout=30)
    subprocess.run([str(binary)], check=True, capture_output=True, timeout=10)
