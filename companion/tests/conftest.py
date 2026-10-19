"""companion/ 를 import 경로에 넣고, 홈 폴더를 임시 폴더로 바꾼다 (실제 사진·설정 폴더를 건드리지 않게)."""

import sys
from pathlib import Path

import pytest

COMPANION = Path(__file__).resolve().parent.parent
if str(COMPANION) not in sys.path:
    sys.path.insert(0, str(COMPANION))


@pytest.fixture(autouse=True)
def fake_home(tmp_path, monkeypatch):
    home = tmp_path / "home"
    home.mkdir()
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("USERPROFILE", str(home))
    return home
