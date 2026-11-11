# 프로젝트 규칙 (LLM Deck, ESP32-S3)

## PC 도우미를 고치면 SD 카드에 설치 파일을 넣는다

`companion/` (S3 PC 도우미)을 고쳤으면, 테스트를 통과한 뒤 설치 파일을 준비한다. 새 PC 에서 기기를 USB 드라이브 모드로 열면
카드의 `LLM-Deck-Helper/` 폴더에서 바로 설치할 수 있게 하려는 것이다.

```sh
companion/.venv/bin/python companion/sd_package.py
```

- Windows·Mac 압축 파일, 설치 방법 `README.txt`, `version.txt` 를 `dist/LLM-Deck-Helper/` 에 만든다.
- 이 Mac 의 도우미가 기기에 USB 로 이어져 있으면 1분 안에 카드의 `version.txt` 와 비교해, 다르면 기기 SD 카드의
  `/LLM-Deck-Helper/` 에 저절로 보낸다 (`companion/sdpkg.py` → 펌웨어 `helper_pkg.cpp`). 사람이 누를 것은 없다.
- 다 들어갔는지는 도우미 기록 `~/.llm-deck/companion.log` 의 `[sd-pkg]` 줄로 확인하고 작업 보고에 적는다.
  카드가 없거나, 녹화 중이거나, USB 가 아니면 넣지 못한다. 그때는 보고에 그렇게 적는다.
- 압축 파일에는 비밀 값(토큰·API 키)을 넣지 않는다. 스크립트가 토큰을 찾아내면 압축 파일을 지운다.
