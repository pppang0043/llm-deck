"""Windows 설치 (windows/install.py, install.cmd): PowerShell 없이 Python 으로 설치한다."""

import importlib.util
from pathlib import Path

WINDOWS = Path(__file__).resolve().parents[1] / "windows"
spec = importlib.util.spec_from_file_location("windows_install", WINDOWS / "install.py")
install = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install)


def test_batch_files_do_not_run_powershell():
    for name in ("install.cmd", "uninstall.cmd", "mirror.cmd", "config.cmd", "orca.cmd"):
        data = (WINDOWS / name).read_bytes()
        commands = "\n".join(line for line in data.decode("utf-8").lower().splitlines()
                              if not line.strip().startswith("rem "))
        assert "powershell" not in commands and ".ps1" not in commands and "executionpolicy" not in commands, name
        # cmd 는 줄바꿈이 CRLF 여야 한다 (LF 면 goto 와 라벨을 잘못 찾는다)
        assert data.count(b"\n") == data.count(b"\r\n"), name
    assert not (WINDOWS / "install.ps1").exists()


def test_read_section_values(tmp_path):
    cfg = tmp_path / "config.toml"
    cfg.write_text("\ufeff[storage]\nmodel_dir = 'D:\\LLM Deck\\models'\ncache_dir = \"D:\\\\cache\"\n"
                   "clean_cache = true\n\n[network]\nfirewall = false\nproxy = \"\"\n", encoding="utf-8")
    assert install.read_section(cfg, "storage") == {"model_dir": "D:\\LLM Deck\\models", "cache_dir": "D:\\cache",
                                                    "clean_cache": True}
    assert install.read_section(cfg, "network") == {"firewall": False, "proxy": ""}
    assert install.read_section(tmp_path / "none.toml", "storage") == {}


def test_add_missing_sections_appends_storage_and_network(tmp_path):
    example = tmp_path / "config.example.toml"
    example.write_text("[device]\nport = ''\n\n# 저장 위치: 설명\n[storage]\nmodel_dir = ''\n\n"
                       "# 회사망: 설명\n[network]\nproxy = ''\n", encoding="utf-8")
    cfg = tmp_path / "config.toml"
    cfg.write_text("[device]\nport = 'COM3'\n\n[network]\nproxy = 'http://p:8080'\n", encoding="utf-8")
    install.add_missing_sections(cfg, example)
    text = cfg.read_bytes().decode("utf-8")
    assert text.count("[storage]") == 1 and text.count("[network]") == 1  # 이미 있는 칸은 그대로
    assert "# 저장 위치: 설명\r\n[storage]\r\nmodel_dir = ''\r\n" in text
    install.add_missing_sections(cfg, example)
    assert cfg.read_bytes().decode("utf-8") == text  # 두 번 해도 같다


def info(exe, version, bits64=True):
    return {"exe": exe, "version": version, "bits64": bits64}


def test_pick_python_newest_supported_with_pythonw(tmp_path):
    def python(folder, pythonw=True):
        d = tmp_path / folder
        d.mkdir(parents=True)
        (d / "python.exe").write_bytes(b"")
        if pythonw:
            (d / "pythonw.exe").write_bytes(b"")
        return str(d / "python.exe")

    p312 = python("Python312")
    p313 = python("Python313")
    p314_no_w = python("Python314", pythonw=False)
    p315 = python("Python315")
    uv = python("uv/python/cpython-3.14")
    best, others = install.pick_python([
        info(p312, (3, 12, 9)), info(p313, (3, 13, 1)), info(p313, (3, 13, 1)), info(p314_no_w, (3, 14, 0)),
        info(p315, (3, 15, 0)), info(python("Python310"), (3, 10, 11)), info(python("x86"), (3, 13, 0), bits64=False),
        info(uv.replace("/", "\\"), (3, 14, 0)),
    ])
    assert best["exe"] == p313
    text = "\n".join(others)
    assert "pythonw.exe 없음" in text and "3.15.0" in text and "3.10.11" in text and "32비트" in text
    assert "uv 가 받은 것" in text


def test_ours_matches_only_this_folders_helper(monkeypatch):
    monkeypatch.setattr(install, "HERE", Path("C:/Deck/app/windows"))
    monkeypatch.setattr(install, "DIR", Path("C:/Deck/app"))
    here = str(install.HERE)
    assert install.ours("pythonw.exe", f'"C:\\Py\\pythonw.exe" "{here}/app.pyw" --background')
    assert install.ours("python.exe", 'python.exe main.py --log "C:\\Users\\me\\.llm-deck\\companion.log"')
    assert install.ours("LLM Deck.exe", "")
    assert not install.ours("pythonw.exe", '"C:\\Py\\pythonw.exe" "D:\\other\\app.pyw"')
    assert not install.ours("python.exe", f'python.exe "{here}/install.py" main.py llm-deck')
    assert not install.ours("notepad.exe", "notepad companion.log main.py llm-deck")


def test_firewall_script_deletes_only_existing_rules():
    script = install.firewall_script(["LLM Deck UDP 4010"], add=True)
    lines = script.split("\r\n")
    assert lines[0] == 'advfirewall firewall delete rule name="LLM Deck UDP 4010"'
    assert 'name="LLM Deck TCP 4011" dir=in action=allow protocol=TCP localport=4011 remoteip=localsubnet' in lines[2]
    assert len([line for line in lines if line]) == 3
    assert install.firewall_script(["LLM Deck UDP 4010", "LLM Deck TCP 4011"], add=False).count("delete") == 2


def test_path_with_keeps_existing_entries():
    folder = Path("C:/Users/me/.local/bin")
    assert install.path_with("%USERPROFILE%\\bin;C:\\Tools", folder) == f"%USERPROFILE%\\bin;C:\\Tools;{folder}"
    assert install.path_with(f"C:\\Tools;{folder}\\", folder) is None
    assert install.path_with("", folder) == str(folder)
