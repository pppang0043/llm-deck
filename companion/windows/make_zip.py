"""Windows 용 도우미 압축 파일(dist/llm-deck-companion-windows.zip)을 만든다.

    companion/.venv/bin/python companion/windows/make_zip.py

git 에 올라간 companion 파일만 담는다. config.toml 은 예시 파일에 이 PC 의 gist_id 만 채워 넣고,
토큰 같은 비밀 값은 넣지 않는다.
"""

import subprocess
import tomllib
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "dist" / "llm-deck-companion-windows.zip"


def main() -> None:
    files = subprocess.run(
        ["git", "ls-files", "companion"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.split()
    gist = ""
    config_path = ROOT / "companion" / "config.toml"
    if config_path.exists():
        gist = tomllib.loads(config_path.read_text(encoding="utf-8")).get("sync", {}).get("gist_id", "")
    example = (ROOT / "companion" / "config.example.toml").read_text(encoding="utf-8")
    config = example.replace('gist_id = ""', f'gist_id = "{gist}"', 1)

    OUT.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            arc = "llm-deck-companion/" + f[len("companion/") :]
            if arc.endswith("windows/README.txt"):
                z.write(ROOT / f, "llm-deck-companion/README.txt")  # 맨 위에도 하나
            z.write(ROOT / f, arc)
        z.writestr("llm-deck-companion/config.toml", config.replace("\n", "\r\n"))

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
