# 공급자 API 비용과 사용률 환산

PR #2 후속 요구(2026-11-28): 비용 페이지는 실제 공급자 API 사용 비용만 표시하고, 구독 사용률의 오른쪽 스와이프는 별도의 토큰 환산 뷰로 제공한다. 최종 카드 결제·구독료·청구서를 조회하는 기능은 아니다.

## API Cost

- 기존 `API Cost` 로컬 월간 환산 수집기를 등록 해제하고 진입점도 비활성화했다. API Cost 화면은 기존 `API Billing` 프로토콜 ID 하나만 사용하며 Anthropic/OpenAI 조직 비용 API만 호출한다.
- 표에는 `YYYY-MM`/오늘, Anthropic/OpenAI를 각각 표시하고, USD·UTC·설정된 조직 범위를 안내한다. 공급자 일부만 조회되더라도 전체 합계나 큰 headline을 만들지 않는다.
- 키 없음은 **미설정**, 401/403은 **인증필요**, 네트워크·미지원 통화·잘못된 응답·불완전한 페이지네이션은 **조회불가**다. 오류 시 예전 성공값, 0달러, 로컬 토큰 추정액으로 대체하지 않는다. 상위 `main.collect_all()`의 예외 경로도 이전 금액 대신 조회불가 페이지를 만든다.
- 정상 응답의 금액은 원문 소수 단위를 Decimal로 합산하며 OpenAI USD와 Anthropic 센트(/100)를 구별한다. 조회 시작은 UTC 월초, 끝은 조회 시각이다. 수동 월 한도/크레딧 추정은 조회하거나 표시하지 않는다.
- Anthropic의 legacy `cost_report`는 Enterprise의 다른 Claude 제품 사용액까지 섞일 수 있어 사용하지 않는다. [공식 Analytics Cost API](https://platform.claude.com/docs/en/api/beta/organization/analytics/cost/list)의 `products[]`를 `messages_api`, `message_batches_api`, `managed_agents_api`로 제한하고 product별 일간 행만 합산한다. 반환된 비API/미식별 product·타통화는 오류로 처리하고 legacy로 되돌아가지 않는다. 해당 제품 필터에 속하지 않는 sandbox/code-execution 등의 미식별 행은 비용 범위에서 제외되므로 공급자 최종 전체 청구액을 의미하지 않는다.
- OpenAI는 [조직 Costs API](https://developers.openai.com/api/reference/resources/admin/subresources/organization/subresources/usage/methods/costs)의 API Platform 비용 범위다. [공식 결제 안내](https://help.openai.com/en/articles/9039756-managing-billing-for-chatgpt-and-the-api-platform)에서 ChatGPT와 API Platform의 별도 결제를 확인했다. 구독이 제공하는 API 크레딧과 실제 크레딧 차감/카드 청구까지 이 화면에서 확인하는 것은 아니다.
- 오늘 bucket이 아직 없으면 **집계대기**로 표시한다. 정상적으로 받은 빈 bucket/월 보고서의 0 사용 비용과 조회 실패를 구별한다. 비용 API의 지연 반영은 남아 있다.
- Anthropic에 아직 비용 export가 없으면 0달러로 처리하지 않는다. Analytics 데이터는 보통 4시간 단위로 갱신되고 뒤늦게 수정될 수 있어, 표시된 값은 해당 API 제품의 공급자 보고 비용이며 확정 청구액이 아니다.

16:19 KST 읽기 전용 실데이터 확인 후 Analytics 제품 필터 적용한 코드로 재조회해 같은 비용/인증 상태를 확인했다:

| 항목 | Anthropic | OpenAI |
| --- | --- | --- |
| 2026-11 API 사용 비용 | 인증필요(401) | $0.58 (원본 $0.58220476 USD) |
| 오늘(UTC) | 인증필요 | 집계대기 |

기존 설정의 키를 메모리로만 읽었고 키 상태 기록 함수도 비활성화해 운영 설정/상태를 쓰지 않았다. 다른 조직·카드 결제·최종 청구 금액은 확인하지 않았다.

## % 사용률 → Token Value → %

- % 페이지에서 **손가락을 오른쪽으로 밀면** 같은 페이지 행의 Token Value로 전환한다. 환산 뷰에서 왼쪽 또는 오른쪽으로 밀면 %로 돌아온다. 세로 페이지 이동과 다른 화면의 기존 오른쪽 뒤로 가기는 유지한다.
- 기존 side tile 프로토콜을 재사용하고 `usage_value=true`로 구분한다. 원래 side tile은 오른쪽 위치로 가기 위해 손가락을 왼쪽으로 밀어야 했고, 오른쪽 밀기는 홈으로 돌아갔다. 이번 구현은 LVGL의 gesture 대상·이벤트 전파를 조정하고 %/환산 tile에서만 명시적으로 전환하며 홈 이벤트를 막는다. 미러링 tileview에도 같은 동작을 연결한다.
- 공급자·선택 계정·5시간/주간 reset 구간과 % snapshot의 `as_of`에 대응하는 **이 PC의 토큰 기록 × 모델의 Standard API 단가**만 합산한다. `% × 구독료/상한` 계산은 없다. 환산 뷰는 “API 환산 · 청구 아님 / 같은 구간의 이 PC 기록”을 표시한다. 다른 PC의 사용량과 기록이 없는 과거 사용분을 포함한 전체 계정 비용으로 해석하면 안 된다. 할인·별도 계약·도구 비용까지 재구성한 청구서가 아니며, 단가 근거는 [공식 OpenAI 가격표](https://developers.openai.com/api/docs/pricing)(2026-11-28 확인)다.
- Orca의 `activeAccountIdsByRuntime.host` 또는 활성 계정 선택과 `providerAccountId`/`workspaceAccountId`를 읽기 전용으로 사용한다. 프로필 로그 폴더는 공용 로그를 가리키는 symlink일 수 있어 **프로필 경로만으로 계정을 판별하지 않는다**.
- Codex의 `session_meta.account_scoped=true`와 `creator_account_id`가 선택 계정 ID에 일치하는 파일만 허용한다. 요청별 `response_id` 중복을 제거하고, reset−구간 길이부터 snapshot 시각까지의 토큰을 합산한다. 알 수 없는 모델을 조용히 빼서 금액을 낮추지 않는다.
- 계정 미대응·공유 snapshot·불명확/만료된 reset·10분 넘게 오래된 snapshot·알 수 없는 단가는 **환산불가**, 해당 구간에 대응하는 기록이 없으면 **기록없음**이다. 월간 추정치를 대신 붙이지 않는다.

같은 시각 실데이터에서 전체 Codex 로그 중 명시적인 account-scoped metadata를 가진 파일 6개를 확인했고, 활성 계정과 일치하는 4개만 사용했다. 해당 snapshot의 5시간 및 주간 로컬 subset 환산은 각각 **$3.83**으로 표시됐다. 이 값은 구독 청구액도, 공급자 API Cost의 $0.58에 더할 금액도 아니다. 로그와 snapshot이 갱신되면 변한다.

16:28 KST 재검증에서도 일치 파일은 4개였고, 그동안 추가된 토큰을 반영한 환산값은 두 구간 모두 **$4.60**이었다. 현재 구현은 Orca가 선택한 host 관리 계정의 명시적 metadata가 있는 Codex 로그만 지원한다. 기본 로그인·다른 PC 공유 snapshot을 관리 계정으로 임의 대응시키지 않는다.

Claude는 현재 로컬 로그에서 사용률 계정과 연결할 수 있는 신뢰할 계정 provenance를 확보하지 못했다. 프로필 폴더도 계정 증거가 되지 않으므로 **환산불가**로 표시한다. 구현을 가능하게 하려면 로그 생성 시점의 인증 계정·조직 ID와 session/response의 결합 정보 또는 공급자가 보장하는 과거 session→계정 매핑이 필요하다. Orca 본체나 원본 로그를 수정하지 않았으며, 과거 계정을 임의 추정하지 않는다.

## 검증·적용

- 최종 전체 companion 테스트 **666 passed**(123.58초), 비용/계정/구간/스와이프 집중 테스트 **46개 통과**. 새 네이티브 C++ 스와이프 테스트 8조건과 symlink 공유 폴더, 선택/다른/미표시·중복 ID 계정, 구간 밖·미래 토큰, stale/reset 오류, 알 수 없는 모델, % 변경 독립성, 조회 실패 후 이전 값 제거, Analytics 제품 필터·헤더·두 페이지·같은 날 다제품 센트 합산·비API 제품 거부·미수출 데이터 거부를 포함한다. 집중 재실행에서 기존 inactive 계정 테스트의 1ms reset이 간헐적으로 만료되는 문제가 드러나 테스트 fixture를 안정적인 미래 구간으로 고쳤다.
- 격리된 worktree에서 **PlatformIO llm-deck 빌드 통과**. Wi-Fi 인증 정보는 복사하지 않고 저장소의 `secrets.example.h`를 사용했다. 새 안내 문구의 한글은 기존 14/20 글꼴에 모두 있다. 물리 기기에 배포하지 않았으므로 실기기 터치·가로/세로 배치의 육안 확인은 미완료다.
- 독립 worktree에서만 작업했으며 운영 도우미·원본 사용 로그·Orca 본체는 변경/재시작하지 않았다. merge·운영 적용은 승인되지 않았고 수행하지 않는다. 업데이트된 펌웨어와 도우미 양쪽 적용이 필요하다.
