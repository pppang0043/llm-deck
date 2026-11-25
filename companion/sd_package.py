"""PC 도우미 설치 파일을 dist/LLM-Deck-Helper/ 에 준비한다. 도우미(sdpkg.py)가 USB 로 이어진 기기 SD 카드에 저절로 넣는다.
새 PC 에서 빠른 설정 > USB 드라이브 로 카드를 열어 카드의 LLM-Deck-Helper 폴더에서 바로 설치하게.

    companion/.venv/bin/python companion/sd_package.py

준비하는 것
    llm-deck-companion-windows.zip  windows/make_zip.py 가 만드는 것과 같다
    llm-deck-companion-mac.zip      companion 폴더 그대로 (테스트·Windows 전용 파일 제외)
    README.txt                      설치 방법, 도우미 버전
    version.txt                     카드에 마지막으로 넣는다. 기기 카드의 것과 다르면 도우미가 모두 다시 보낸다
config.toml 은 예시 파일에 이 PC 의 gist_id 만 채우고 토큰 같은 비밀 값은 넣지 않는다. 음성 인식 모델도 넣지 않는다.
"""

import hashlib
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "windows"))
import make_zip  # noqa: E402

ROOT = make_zip.ROOT
MAC_OUT = ROOT / "dist" / "llm-deck-companion-mac.zip"
STAGE = ROOT / "dist" / "LLM-Deck-Helper"
# Mac 에서 쓰지 않는 것 (Windows 설치, 이 Mac 에서만 돌리는 도구, 테스트)
MAC_SKIP = ("companion/windows/", "companion/tests/", "companion/sd_package.py")

README = """LLM Deck PC 도우미 설치 파일
============================

도우미 버전 {version} ({date} 에 넣음)

Windows
  1. llm-deck-companion-windows.zip 을 PC 로 복사해 풉니다 (카드 안에서 바로 풀지 않습니다).
  2. 맨 위의 install.cmd 를 더블클릭합니다. 자세한 내용은 압축 안의 INSTALL.txt.
     64비트 Python 3.11~3.14 와 처음 설치할 때 인터넷 연결이 필요합니다.

Mac
  1. llm-deck-companion-mac.zip 을 PC 로 복사해 풉니다 (예: ~/llm-deck-companion).
  2. 터미널에서
       xcode-select --install          (처음 한 번. 이미 있으면 건너뜀)
       curl -LsSf https://astral.sh/uv/install.sh | sh   (uv 가 없을 때)
       cd ~/llm-deck-companion
       uv sync
       sh macos/install.sh
  3. 메뉴 막대의 LLM Deck 아이콘에서 미러링 창을 엽니다. 블루투스·손쉬운 사용 허용 창이 뜨면 허용합니다.

공통
  - 설정 파일은 config.toml (Windows 는 app\\config.toml) 입니다. 비밀 값(토큰·API 키)은 들어 있지 않으니 필요하면 직접 넣습니다.
  - 기기와 처음 한 번은 USB 로 연결해야 Wi-Fi·블루투스 인증 키를 받습니다.
    이 카드를 USB 드라이브로 연 동안은 USB 연결이 저장장치로 바뀌어 있으니, 복사가 끝나면 PC 에서 꺼내기 한 뒤
    기기 화면의 "끝내고 다시 시작" 을 누릅니다.
"""


def build_mac() -> Path:
    files = make_zip.companion_files(MAC_SKIP)
    files = [f for f in files if not f.endswith(".exe")]
    big = [f for f in files if f.endswith((".bin", ".zip", ".safetensors")) or (ROOT / f).stat().st_size > 2_000_000]
    if big:
        raise SystemExit(f"모델 같은 큰 파일은 넣지 않습니다: {big}")
    config, _ = make_zip.config_text()
    MAC_OUT.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(MAC_OUT, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            z.write(ROOT / f, "llm-deck-companion/" + f[len("companion/") :])
        z.writestr("llm-deck-companion/config.toml", config)
        z.writestr("llm-deck-companion/VERSION", make_zip.helper_version())
    make_zip.check_no_token(MAC_OUT)
    print(f"{MAC_OUT} ({len(files)}개 파일)")
    return MAC_OUT


def stage(folder: Path, zips: list[Path], version: str) -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    for z in zips:
        data = z.read_bytes()
        digest.update(data)
        (folder / z.name).write_bytes(data)
    text = README.format(version=version, date=time.strftime("%Y-%m-%d %H:%M"))
    # Windows 메모장에서도 바로 읽히게 BOM 과 CRLF
    (folder / "README.txt").write_bytes("\ufeff".encode() + text.replace("\n", "\r\n").encode("utf-8"))
    (folder / "version.txt").write_text(f"{version} {digest.hexdigest()[:8]}\n")
    return folder


def main() -> None:
    make_zip.main()
    folder = stage(STAGE, [make_zip.OUT, build_mac()], make_zip.helper_version())
    print(f"{folder} 에 준비했습니다. 도우미가 USB 로 이어진 기기 SD 카드에 넣습니다 (도우미 기록 [sd-pkg])")


if __name__ == "__main__":
    main()
