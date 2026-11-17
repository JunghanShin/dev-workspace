# 모듈 `source` (예제: 데이터 만들기)

주기마다 숫자 배열(표본)을 만들어 내보낸다. 실제 워커에서는 장비에서 데이터를 받는 자리(카메라 프레임, 센서 값, 장치 메시지 등)다.

| 항목 | 내용 |
|---|---|
| 유닛(코어) | `Generator`(`core/`): 주기 판정, 표본 만들기. libworker 없음 |
| 스레드 | `source.gen`(표본 만들기 → 데이터 버스에 쓰기), `source.pub`(스트림 발행) |
| 포트(워커 안) | `SourcePort`(`include/source/port.hpp`): 같은 워커의 다른 모듈이 표본을 받음 |
| 워커 간 | 데이터 버스(`Format::raw`, 다른 워커가 InOrder/Latest로 받음), 스트림 주제 `source`, 명령 |
| 설정 | `config.source`: `rate_hz`(바로 적용), `size`, `amplitude`(바로 적용), `bus_slots`, `bus_socket`, `helper_cmd` |
| 명령 | `source.set_rate {hz}`(바로, 저장 안 함), `source.reset` |
| 상태 | `get_status` → `data.source`: `seq`, `rate_hz`, `mean`, `bus`, `helper` |
| 지표 | `source_seq`, `source_rate_hz`, `source_mean`, `source_bus_consumers`, `source_drops`, `source_link`, `source_helper_up` |
| 자식 프로세스 | `helper_cmd`가 있으면 띄우고 감시(죽으면 2초 뒤 다시). 워커가 끝나면 함께 끝남 |

시험: `test/test_source.cpp`(유닛 + 호스트 없는 모듈)
