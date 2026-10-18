LLM Deck PC 도우미 (Windows)
============================

LLM Deck 기기에 사용량을 보내고, 기기와 PC 를 이어 주는 프로그램입니다.
창 없이 뒤에서 돌고, 로그인할 때 자동으로 켜집니다.


누를 파일
---------
  install.cmd    설치 (고친 뒤 다시 켤 때도 이것)
  uninstall.cmd  제거
  mirror.cmd     기기 화면을 브라우저로 보고 눌러서 조작 (USB·Wi-Fi 연결일 때)
                 주소는 늘 http://127.0.0.1:4013/ 이라 즐겨찾기해 둬도 됩니다.
  app 폴더       도우미 본체. 건드리지 않아도 됩니다.


설치
----
1. 이 폴더를 지우지 않을 곳에 둡니다. (예: C:\Users\내이름\llm-deck-companion)
   설치 뒤에도 이 폴더에서 실행되므로, 옮기거나 지우면 다시 설치해야 합니다.
   예전 버전이 있으면 그 폴더의 uninstall.cmd(예전엔 windows 폴더 안)를 먼저 누르고 폴더를 지웁니다.
2. install.cmd 를 더블클릭합니다.
   - "Windows의 PC 보호" 창이 뜨면 [추가 정보] > [실행]
   - 관리자 확인 창이 뜨면 [예] (Wi-Fi 연결용 방화벽 허용)
   - 처음에는 필요한 부품을 받느라 몇 분 걸립니다.
3. "설치했습니다" 가 나오면 끝입니다.

기기와는 USB 케이블, 블루투스, 같은 Wi-Fi 중 되는 것으로 알아서 연결합니다.
블루투스는 PC 블루투스를 켜 두기만 하면 됩니다. (Windows 에서 따로 페어링하지 않습니다)


저장 위치
---------
  사진     사진\LLM Deck
  동영상   비디오\LLM Deck   (.mov 가 안 열리면 VLC 로)
  기록     C:\Users\내이름\.llm-deck\companion.log
  설정     app\config.toml   (메모장으로 고친 뒤 install.cmd 다시 실행)

여러 PC 의 한도 사용률은 설정의 gist 로 함께 봅니다 (미리 채워 둠). 받기만 할 때는 할 일이 없고,
이 PC 의 값을 올리려면 GitHub CLI 로그인(gh auth login -s gist)이 필요합니다.


음성 질문이 안 될 때
--------------------
- "Windows 가 마이크를 막았습니다"
    Windows 설정 > 개인 정보 및 보안 > 마이크 에서
    "마이크 액세스" 와 "데스크톱 앱이 마이크에 액세스하도록 허용" 을 둘 다 켭니다.
- "PC 에 마이크가 없습니다"
    헤드셋·웹캠 마이크를 꽂으면 30초 안에 다시 켜집니다.
- "음성 질문 부품을 켜지 못했습니다" (설치할 때)
    https://aka.ms/vs/17/release/vc_redist.x64.exe 를 설치하고 install.cmd 를 다시 실행합니다.
- "음성 인식 모델을 불러오지 못했습니다" (회사망이 막을 때)
    Mac 에서 만든 llm-deck-whisper-small.zip 을 USB 로 옮겨 풀고,
    안의 faster-whisper-small 폴더를 통째로 아래에 넣은 뒤 install.cmd 를 다시 실행합니다.
      C:\Users\내이름\.llm-deck\models\faster-whisper-small   (안에 model.bin 이 바로 보여야 함)
    (Mac 에서 만들기: companion/.venv/bin/python companion/windows/pack_model.py)
