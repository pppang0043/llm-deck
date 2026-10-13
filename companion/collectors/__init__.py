"""서비스별 사용량 수집기.

각 수집기는 collect(cfg) 함수를 가지며, 기기 화면 한 페이지에 해당하는 dict 를 돌려준다:
    {"name": 제목, "note": 값의 시각, "headline": 큰 글자, "sub": 설명,
     "bars": [{"label", "note", "text", "pct"}], "rows": [[이름, 값], ...]}
표시할 것이 없으면(키 미설정 등) None 을 돌려준다.
"""

from . import anthropic_api, claude_code, codex, openai_api

# (설정 섹션 이름, 모듈, 최소 호출 간격 초). 화면에 이 순서대로 나온다.
COLLECTORS = [
    ("claude_code", claude_code, 0),
    ("codex", codex, 0),
    ("anthropic_api", anthropic_api, 300),
    ("openai_api", openai_api, 300),
]

