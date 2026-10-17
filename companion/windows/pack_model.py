"""회사 PC 처럼 Hugging Face 에서 음성 인식 모델을 받지 못하는 PC 에 옮길 모델 압축 파일을 만든다 (Mac 에서 실행).

    companion/.venv/bin/python companion/windows/pack_model.py          # small (약 480MB)
    companion/.venv/bin/python companion/windows/pack_model.py base     # config 의 whisper_model 과 같은 이름으로

dist/llm-deck-whisper-<이름>.zip 이 생긴다. USB 로 옮겨 Windows PC 에서 압축을 풀고,
안의 faster-whisper-<이름> 폴더를 C:\\Users\\내이름\\.llm-deck\\models\\ 아래에 넣는다.
도우미(assistant.py)는 그 폴더가 있으면 인터넷으로 받지 않고 그것을 쓴다.
"""

import sys
import zipfile
from pathlib import Path

from faster_whisper import WhisperModel
from faster_whisper.utils import download_model

ROOT = Path(__file__).resolve().parents[2]
GUIDE = """이 압축 파일 안의 {folder} 폴더를 통째로 아래에 넣으세요.

    C:\\Users\\내이름\\.llm-deck\\models\\{folder}

(.llm-deck 폴더는 도우미를 설치하면 생깁니다. models 폴더가 없으면 만드세요.)
폴더 안에 model.bin 이 바로 보여야 합니다. 넣은 뒤 도우미를 다시 켜면(로그아웃 후 로그인, 또는 install.cmd)
기록 파일에 "음성 인식 모델 불러오는 중 (...\\.llm-deck\\models\\{folder})" 가 나옵니다.
"""


def main() -> None:
    name = sys.argv[1] if len(sys.argv) > 1 else "small"
    folder = f"faster-whisper-{name}"
    out_dir = ROOT / "dist" / folder
    print(f"받는 중: {name} -> {out_dir}")
    download_model(name, output_dir=str(out_dir))
    # Hugging Face 가 남기는 내려받기 기록(.cache)은 빼고 모델 파일만 담는다
    files = sorted(p for p in out_dir.iterdir() if p.is_file())
    if not any(p.name == "model.bin" for p in files):
        raise SystemExit("model.bin 을 받지 못했습니다")
    # 실제로 불러지는지 확인한다 (Windows 와 같은 설정)
    WhisperModel(str(out_dir), device="cpu", compute_type="int8")

    zip_path = ROOT / "dist" / f"llm-deck-whisper-{name}.zip"
    # 모델은 이미 압축된 숫자라 더 줄지 않는다. 그냥 담아 빨리 만든다
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_STORED) as z:
        for p in files:
            z.write(p, f"{folder}/{p.name}")
        z.writestr("README.txt", GUIDE.format(folder=folder).replace("\n", "\r\n").encode("utf-8-sig"))
    size = zip_path.stat().st_size / 1e6
    print(f"{zip_path} ({size:.0f}MB, {', '.join(p.name for p in files)})")


if __name__ == "__main__":
    main()
