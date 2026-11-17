# 모듈 `peer` (예제: 워커 간 받기)

설정 `peer`(다른 워커의 id)가 내보내는 것을 받는다. 비우면 아무것도 하지 않는다. 워커 간 통신의 본보기(워크스페이스 design.md 6.6.7).

| 통로 | 도구 | 스레드 |
|---|---|---|
| 큰 데이터: 상대 데이터 버스를 순서대로 전부 | `framebus::Consumer(InOrder)` | `peer.data` |
| 신호·상태: 상대 스트림 구독 | `worker::Subscriber` | `peer.sub` |
| 요청·응답: 상대에 명령(비동기) | `worker::CommandClient` | `peer.cmd` |

| 항목 | 내용 |
|---|---|
| 유닛(코어) | `SeqTracker`(`core/`, 헤더): 받은 번호로 받은 수·빠진 수·상대 다시 시작 |
| 설정 | `config.peer.peer`(형식 `worker_id`), `bus_socket` |
| 명령 | `peer.set_rate {hz}` → 상대의 `source.set_rate`(응답은 상대의 것), `peer.reset` |
| 상태 | `data.peer`: `data_link`, `stream_link`, `received`, `lost`, `peer_restarts`, `peer_topics` |
| 지표 | `peer_link`, `peer_received`, `peer_lost` |
| 상대가 없을 때 | 살아서 기다리며 다시 붙는다(`peer_link=0`). 상대를 시작·정지하지 않는다 |

시험: `test/test_peer.cpp`(유닛 + 호스트 없는 모듈). 실제 워커 간 통신은 워커 시험(`test/test_worker.cpp`)
