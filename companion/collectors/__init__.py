"""서비스별 사용량 수집기. 두 종류가 있다.

한도 수집기(LIMITS): snapshot(cfg) 가 구독 한도 사용률을 돌려준다.
    {"as_of": 값의 시각, "windows": [[이름, 사용률 %, 초기화 시각], ...]}
    이 값은 sync.py 를 거쳐 다른 PC 와 공유된다.

API 수집기(COLLECTORS): collect(cfg) 가 기기 화면 한 페이지에 해당하는 dict 를 돌려준다.
    {"name": 제목, "note": 값의 시각, "headline": 큰 글자, "sub": 설명,
     "bars": [{"label", "note", "text", "pct"}], "rows": [[이름, 값], ...]}

표시할 것이 없으면(키 미설정 등) None 을 돌려준다.
"""

from . import anthropic_api, api_cost, claude_code, codex, openai_api

# (설정 섹션 이름, 모듈). 화면에 이 순서대로 나온다.
LIMITS = [
    ("claude_code", claude_code),
    ("codex", codex),
]

# (설정 섹션 이름, 모듈, 최소 호출 간격 초). 한도 페이지 뒤에 이 순서대로 나온다.
COLLECTORS = [
    ("api_cost", api_cost, 300),
    ("anthropic_api", anthropic_api, 300),
    ("openai_api", openai_api, 300),
]
