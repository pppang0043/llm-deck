"""Anthropic / OpenAI 실제 API 청구액을 한 페이지에 표로 나란히 보여준다.

    API Billing          11:44
               Anthropic  OpenAI
    10월           $0.36   $0.42
    오늘           $0.00   $0.18
    한도 남음     $99.64   $9.58
    크레딧        $99.52   $9.34

키를 넣은 회사만 열이 생긴다. 한도·크레딧 줄은 어느 한 회사라도 값이 있을 때만 나온다.
"""

from datetime import datetime

from . import anthropic_api, openai_api
from .fmt import fmt_usd

# (설정 섹션 이름, 모듈). 표의 열 순서
PROVIDERS = [("anthropic_api", anthropic_api), ("openai_api", openai_api)]

# 섹션 -> 마지막으로 받은 값. 잠깐 인터넷이 끊겨도 빈칸 대신 앞 값을 보여준다
_last: dict[str, dict] = {}


def _usd(value: float | None) -> str:
    if value is None:
        return "-"
    return fmt_usd(value) if value >= 0 else "-" + fmt_usd(-value)


def collect(cfg: dict) -> dict | None:
    columns = []
    for section, module in PROVIDERS:
        try:
            result = module.fetch(cfg.get(section, {}))
        except Exception as e:  # 한 회사가 실패해도 다른 회사는 보여준다
            print(f"[{section}] 수집 실패: {e}")
            result = _last.get(section)
        if result is None:
            continue
        if "error" not in result:
            _last[section] = result
        columns.append((module.NAME, result))
    if not columns:
        return None

    def row(label: str, field: str) -> list:
        return [label] + [_usd(result.get(field)) for _, result in columns]

    now = datetime.now()
    table = [[""] + [name for name, _ in columns], row(f"{now.month}월", "month"), row("오늘", "today")]
    for label, field in (("한도 남음", "limit_left"), ("크레딧", "credit_left")):
        if any(result.get(field) is not None for _, result in columns):
            table.append(row(label, field))

    notes = [f"{name}: {result['error']}" for name, result in columns if "error" in result]
    notes += [f"{name}: {result['note']}" for name, result in columns if result.get("note")]
    page = {"name": "API Billing", "note": now.strftime("%H:%M"), "table": table}
    if notes:
        page["sub"] = "\n".join(notes)
    return page
