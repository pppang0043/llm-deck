"""기기 없이 실제 C++ 큐의 폭주·할당 실패·응답 크기 제한을 검증한다."""
import shutil
import subprocess
from pathlib import Path

import pytest


@pytest.mark.parametrize("source", ["message_queue_test", "command_journal_test", "control_policy_test"])
def test_native_queue_and_response_limits(tmp_path, source):
    firmware = Path(__file__).resolve().parents[2] / "firmware"
    compiler = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if not compiler or not firmware.exists():
        pytest.skip("C++ 컴파일러와 펌웨어 소스가 필요합니다")
    binary = tmp_path / "queue-test"
    includes = ["-I", str(firmware / "src")]
    if source in {"command_journal_test", "control_policy_test"}:
        arduino_json = firmware / ".pio/libdeps/llm-deck/ArduinoJson/src"
        if not arduino_json.exists():
            pytest.skip("PlatformIO 의 ArduinoJson 헤더가 필요합니다")
        includes += ["-I", str(arduino_json)]
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    *includes, str(firmware / f"tests/{source}.cpp"),
                    "-o", str(binary)], check=True, capture_output=True, text=True, timeout=30)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
