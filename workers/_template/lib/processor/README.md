# 모듈 `processor` (예제: 워커 안 처리)

같은 워커의 `source` 모듈이 만든 표본을 포트(`SourcePort`)로 받아 최근 N개의 평균·최소·최대를 낸다. 실제 워커에서는 받은 데이터를 처리하는 자리다.

| 항목 | 내용 |
|---|---|
| 유닛(코어) | `Window`(`core/`): 최근 N개 값 |
| 포트 | `source/port.hpp`만 안다(모듈 사이 연결 본보기). 같은 포트를 구현한 다른 모듈로 바꿔 끼울 수 있음 |
| 스레드 | 없음(포트 콜백은 워커 루프 안, 짧게) |
| 설정 | `config.processor.window`(바로 적용) |
| 명령 | `processor.reset` |
| 상태·지표 | `data.processor`: `avg`, `min`, `max`, `count`, `total` / `processor_avg`, `processor_min`, `processor_max`, `processor_total` |

시험: `test/test_processor.cpp`(유닛 + 가짜 포트로 모듈)
