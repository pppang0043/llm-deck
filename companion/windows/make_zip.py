"""Windows 용 도우미 압축 파일(dist/llm-deck-companion-windows.zip)을 만든다.

    companion/.venv/bin/python companion/windows/make_zip.py

git 이 아는 companion 파일(커밋 전 새 파일 포함, .gitignore 에 든 것은 뺀다)만 담는다. config.toml 은 예시 파일에 이 PC 의 gist_id 만 채워 넣고,
토큰 같은 비밀 값은 넣지 않는다.

풀었을 때 맨 위에는 누를 파일과 README 만 두고, 도우미 코드는 app 폴더에 넣는다.
    llm-deck-companion/  install.cmd  uninstall.cmd  mirror.cmd  config.cmd  README.txt  INSTALL.txt  app/
음성 인식(Whisper) 모델은 넣지 않는다. 받는 방법은 INSTALL.txt 에 적어 두었다.
"""

import subprocess
import tomllib
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "dist" / "llm-deck-companion-windows.zip"
TOP = ("install.cmd", "uninstall.cmd", "mirror.cmd", "config.cmd", "README.txt", "INSTALL.txt")
# Windows 에서 쓰지 않는 것 (Mac 설치, 이 Mac 에서만 돌리는 도구)
SKIP = ("companion/macos/", "companion/windows/make_zip.py", "companion/windows/pack_model.py")


def main() -> None:
    files = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "companion"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.split()
    files = [f for f in files if (ROOT / f).is_file() and not f.startswith(SKIP)]  # 지웠지만 아직 커밋하지 않은 파일도 뺀다
    # 미러링 앱(LLM Deck Mirror.exe)은 설치할 때 mirror.cs 를 컴파일해 만드므로 넣지 않는다
    files = [f for f in files if not f.endswith(".exe")]
    # 음성 인식 모델(model.bin 등 큰 파일)이 섞여 들어가지 않게 막는다
    big = [f for f in files if f.endswith((".bin", ".zip", ".safetensors")) or (ROOT / f).stat().st_size > 2_000_000]
    if big:
        raise SystemExit(f"모델 같은 큰 파일은 넣지 않습니다: {big}")
    gist = ""
    config_path = ROOT / "companion" / "config.toml"
    if config_path.exists():
        gist = tomllib.loads(config_path.read_text(encoding="utf-8")).get("sync", {}).get("gist_id", "")
    example = (ROOT / "companion" / "config.example.toml").read_text(encoding="utf-8")
    config = example.replace('gist_id = ""', f'gist_id = "{gist}"', 1)

    OUT.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            name = f[len("companion/") :]
            if name.startswith("windows/") and name[len("windows/") :] in TOP:
                arc = "llm-deck-companion/" + name[len("windows/") :]
            else:
                arc = "llm-deck-companion/app/" + name
            z.write(ROOT / f, arc)
        z.writestr("llm-deck-companion/app/config.toml", config.replace("\n", "\r\n"))

    # 토큰이 섞여 들어가지 않았는지 확인한다
    with zipfile.ZipFile(OUT) as z:
        # 이 파일 자신에 걸리지 않도록 찾을 글자를 나눠 적는다
        prefixes = [p.encode() + b"_" for p in ("ghp", "gho", "github_pat")]
        leaked = [n for n in z.namelist() if any(t in z.read(n) for t in prefixes)]
    if leaked:
        OUT.unlink()
        raise SystemExit(f"토큰으로 보이는 값이 있어 지웠습니다: {leaked}")
    print(f"{OUT} ({len(files)}개 파일, gist {'있음' if gist else '없음'})")


if __name__ == "__main__":
    main()
