"""받아 둔 부품 파일(wheel)을 도우미 부품 폴더(app\\lib)에 푼다 (Windows 설치, install.ps1 이 부른다).

    python unpack_wheels.py <wheel 폴더> <풀 폴더>

pip install 은 부품마다 명령 실행 파일(f2py.exe, tqdm.exe 같은 서명 없는 exe)을 새로 만든다. 회사 보안 프로그램이
처음 보는 exe 라고 막으므로, pip 로는 받기만 하고 (pip download, 파일 지문 확인) 여기서 직접 푼다.
wheel 은 zip 파일이라 그대로 풀면 되고, 명령 실행 파일(.data/scripts, entry_points)은 만들지 않는다.
도우미는 부품의 명령을 쓰지 않고 Python 안에서 import 만 한다.
"""

import shutil
import sys
import zipfile
from pathlib import Path

# 풀지 않는 것: 실행 파일과 스크립트 (부품 안에 들어 있어도). 도우미는 쓰지 않는다
BLOCKED = (".exe", ".bat", ".cmd", ".ps1", ".vbs", ".com", ".scr", ".msi")


def unpack(wheel: Path, target: Path) -> list[str]:
    """wheel 하나를 target 에 푼다. 풀지 않고 건너뛴 파일 이름을 돌려준다."""
    skipped = []
    with zipfile.ZipFile(wheel) as z:
        for info in z.infolist():
            name = info.filename
            if info.is_dir():
                continue
            parts = name.split("/")
            # 이름.data/purelib|platlib/... 은 그 아래를 바로 풀고, scripts·headers·data 는 쓰지 않는다
            if parts[0].endswith(".data") and len(parts) > 2:
                if parts[1] not in ("purelib", "platlib"):
                    skipped.append(name)
                    continue
                parts = parts[2:]
            if name.lower().endswith(BLOCKED):
                skipped.append(name)
                continue
            # 폴더 밖으로 나가는 이름은 풀지 않는다
            if any(p in ("", "..") for p in parts) or ":" in parts[0]:
                skipped.append(name)
                continue
            out = target.joinpath(*parts)
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)
    return skipped


def main() -> None:
    wheels, target = Path(sys.argv[1]), Path(sys.argv[2])
    files = sorted(wheels.glob("*.whl"))
    if not files:
        raise SystemExit(f"부품 파일이 없습니다: {wheels}")
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    skipped = []
    for wheel in files:
        skipped += unpack(wheel, target)
    left = [str(p) for p in target.rglob("*") if p.suffix.lower() in BLOCKED]
    if left:
        raise SystemExit(f"실행 파일이 남았습니다: {left}")
    # 이 부품은 이 Python 버전용이다 (다른 버전으로 바꾸면 install.cmd 를 다시 실행해야 한다)
    (target / "python-version.txt").write_text("%d.%d" % sys.version_info[:2], encoding="utf-8")
    print(f"      부품 {len(files)}개를 풀었습니다 (실행 파일·스크립트 {len(skipped)}개는 만들지 않음)")


if __name__ == "__main__":
    main()
