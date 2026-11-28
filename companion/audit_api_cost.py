"""로컬 API 환산치 재현 (읽기 전용, 키/대화/응답 ID 출력 없음).

실행 중 도우미와 같은 로그 경로를 --claude-root / --codex-root 로 지정한다.
Orca worker 의 프로필 환경은 실행 중 도우미의 환경과 다를 수 있다.
"""

import argparse
import json
from datetime import datetime
from pathlib import Path

from collectors import api_cost


def audit(month: str, claude_root: Path, codex_root: Path) -> dict:
    services = {}
    for name, root, reader in (("Claude", claude_root, api_cost._read_claude),
                               ("Codex", codex_root, api_cost._read_codex)):
        models = {}
        for date, usd, model in api_cost._records(root, reader).values():
            if not date.startswith(month):
                continue
            item = models.setdefault(model, {"responses": 0, "estimated_usd": 0.0})
            item["responses"] += 1
            if usd is None:
                item["estimated_usd"] = None
            elif item["estimated_usd"] is not None:
                item["estimated_usd"] += usd
        services[name] = {"root": str(root), "models": models,
                          "estimated_usd": sum(v["estimated_usd"] or 0 for v in models.values())}
    return {"month": month, "timezone": str(datetime.now().astimezone().tzinfo),
            "kind": "API-equivalent estimate, includes subscription usage; NOT a bill",
            "services": services}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--month", default=datetime.now().strftime("%Y-%m"))
    parser.add_argument("--claude-root", type=Path, default=api_cost.CLAUDE_ROOT)
    parser.add_argument("--codex-root", type=Path, default=api_cost.CODEX_ROOT)
    args = parser.parse_args()
    try:
        valid = datetime.strptime(args.month, "%Y-%m").strftime("%Y-%m") == args.month
    except ValueError:
        valid = False
    if not valid:
        parser.error("--month must be YYYY-MM")
    print(json.dumps(audit(args.month, args.claude_root, args.codex_root), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
