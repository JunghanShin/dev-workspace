# 남은 일 목록 (매니저 첫 판 뒤)

매니저 첫 판(0.62~0.71) 뒤에 남은 일과 순서. 설계는 각 항목이 들어가는 곳의 설계서에 따로 둔다(이 문서는 목록과 진행 상황만). 새 동작은 모두 규칙·계약 표시가 없으면 꺼짐(지금과 같음).

| # | 일 | 들어가는 곳 | 설계서 | 상태 |
|---|---|---|---|---|
| 1 | 명령 임대(끊기면 안전 상태) | 계약 + libworker | `docs/command-lease.md` | 설계 중 |
| 2 | 외부 연동 워커를 mission으로 | mission-manager, process-manager | `managers/mission/design.md` 다음 판 | 대기 |
| 3 | 우선순위(누가 이기나) | mission-manager | `managers/mission/design.md` 다음 판 | 대기 |
| 4 | 명령 조건(모드 전환 허가) | mission-manager | `managers/mission/design.md` 다음 판 | 대기 |
| 5 | fault 조건 조합·단계 대응 | fault-manager | `managers/fault/design.md` 다음 판 | 대기 |
| 6 | 받은 칸 실행 중 검사 | libworker | `docs/input-check.md` | 대기 |
| 7 | 워크스페이스 화면(건강 띠, 패널 "시스템") | `ui/web` | `ui/README.md`·패널 설계 | 대기 |
| 8 | HTTPS 인증서 | 설치, nginx, 게이트웨이 | `docs/gateway.md` | 대기 |

순서 이유: 안전(임대)과 명령 입구 하나(외부 연동 워커)를 먼저, 그 위에 mission 규칙(우선순위·조건), 판단 확장(fault), 품질(검사), 보이기(화면), 마지막에 배포 쪽(HTTPS). 항목마다 설계 → 구현 → 시험 → 보드 확인 뒤 다음으로.

끝난 것: 명령 검사 `enforce`(0.71), 시스템 모니터 libworker 0.14.
