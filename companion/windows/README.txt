LLM Deck PC 도우미 (Windows)
================================

LLM Deck 기기에 사용량을 보내고, 기기의 마이크·카메라·알림음을 PC 와 연결하는 프로그램입니다.
창 없이 뒤에서 돌고, 로그인할 때 자동으로 켜집니다.


설치
----
1. 이 폴더를 지우지 않을 곳에 둡니다. (예: C:\Users\내이름\llm-deck-companion)
   설치 뒤에도 이 폴더에서 실행되므로, 옮기거나 지우면 다시 설치해야 합니다.
2. windows 폴더의 install.cmd 를 더블클릭합니다.
   - 처음에는 Python 도구(uv)와 필요한 부품을 받느라 몇 분 걸립니다.
   - "Windows의 PC 보호" 창이 뜨면 [추가 정보] > [실행] 을 누릅니다.
3. "설치했습니다" 가 나오면 끝입니다.


기기 연결
---------
도우미는 아래 중 되는 것으로 자동 연결합니다.
- USB 케이블 (가장 빠름. 동영상 녹화는 USB 또는 Wi-Fi 일 때만 됩니다)
- 블루투스: PC 블루투스를 켜 두기만 하면 됩니다. (Windows 설정에서 따로 "페어링" 하지 않습니다)
- Wi-Fi: 기기와 PC 가 같은 공유기에 연결되어 있을 때


무엇이 되나요
-------------
- 사용량: 이 PC 의 Claude Code / Codex 한도 사용률. Claude Code 가 없는 PC 는 다른 PC 가 올린 값을 받아 옵니다.
- 사진·동영상: 기기 카메라로 찍으면 이 PC 의 "사진\LLM Deck", "비디오\LLM Deck" 폴더에 저장됩니다.
  동영상(.mov)이 Windows 기본 앱에서 재생되지 않으면 VLC 플레이어로 열어 주세요.
  폴더가 없으면 자동으로 만듭니다. Windows 보안의 "제어된 폴더 액세스"가 막으면
  대신 C:\Users\내이름\.llm-deck\photos (동영상은 videos) 에 저장하고 기록 파일에 알려 줍니다.
- 알림음: 기기 마이크를 켜고 끌 때 PC 스피커로 짧은 소리가 납니다.
- 음성 질문: 기기에서 버튼을 누르고 말하면 이 PC 마이크로 받아, 이 PC 안에서 글자로 바꾼 뒤
  Claude Code(claude-test 또는 claude 명령)에 묻고 답을 기기에 보여 줍니다. 소리는 인터넷으로 나가지 않습니다.
  PC 에 마이크가 없거나 Windows 가 막아 두면 기기의 말하기 버튼이 흐리게 바뀝니다. (아래 "음성 질문이 안 될 때")
- 마이크: 기기에 마이크 부품을 단 뒤, VB-CABLE(https://vb-audio.com/Cable/)을 설치하면
  다른 프로그램에서 "CABLE Output" 을 마이크로 고를 수 있습니다.


설정
----
config.toml 을 메모장으로 열어 고칩니다. 고친 뒤에는 PC 를 다시 로그인하거나 install.cmd 를 다시 실행합니다.
- [sync] gist_id: 여러 PC 의 한도 사용률을 함께 쓰는 곳. 미리 채워 두었습니다.
  이 PC 의 값을 올리려면 GitHub CLI 로그인(gh auth login -s gist)이 필요합니다. 받기만 할 때는 필요 없습니다.


문제가 있을 때
--------------
- 기록 파일: C:\Users\내이름\.llm-deck\companion.log
- 지우기: windows 폴더의 uninstall.cmd 를 더블클릭한 뒤, 이 폴더를 지웁니다.


음성 질문이 안 될 때
--------------------
- "Windows 가 마이크를 막았습니다": Windows 설정 > 개인 정보 및 보안 > 마이크 에서
  "마이크 액세스" 와 "데스크톱 앱이 마이크에 액세스하도록 허용" 을 둘 다 켭니다.
- "PC 에 마이크가 없습니다": 헤드셋·웹캠 마이크를 꽂으면 30초 안에 다시 켜집니다.
  마이크가 여러 개면 config.toml 의 [assistant] input 에 쓸 마이크 이름 일부를 적습니다.
- "마이크 소리가 들어오지 않습니다": 마이크 음소거(헤드셋 버튼, 작업 표시줄 소리 설정)를 풉니다.
- "음성 인식 모델을 불러오지 못했습니다": 처음 질문할 때 인터넷(Hugging Face)에서 모델을 받는데,
  회사망이 막으면 받지 못합니다. 이럴 때는 집 Mac 에서 모델을 받아 USB 로 옮깁니다.
    1) Mac 에서: companion/.venv/bin/python companion/windows/pack_model.py
       -> dist/llm-deck-whisper-small.zip (약 480MB) 이 생깁니다.
    2) 이 PC 에서 압축을 풀고, 안의 faster-whisper-small 폴더를 통째로 아래에 넣습니다.
         C:\Users\내이름\.llm-deck\models\faster-whisper-small
       (폴더 안에 model.bin 이 바로 보여야 합니다.)
    3) install.cmd 를 다시 실행합니다. 기록 파일에 "음성 인식 모델 불러오는 중 (...models\faster-whisper-small)" 이 나오면 됩니다.
  config.toml 의 whisper_model 을 base 처럼 바꿨다면, 1) 에서 pack_model.py base 로 받고 폴더 이름도 faster-whisper-base 입니다.
- "음성 질문 부품을 켜지 못했습니다"(설치할 때): Microsoft Visual C++ 재배포 가능 패키지(x64)를 설치합니다.
  https://aka.ms/vs/17/release/vc_redist.x64.exe
