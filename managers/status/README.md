# status-manager

시스템 상태 매니저(기본 탑재). 설계는 `design.md`, 매니저 공통 틀은 `../../docs/managers.md`.

워커 프로세스 상태·heartbeat·링크 지표로 워커마다 건강(`ok`·`degraded`·`fault`·`starting`·`off`)을 정하고, 시스템 건강 하나로 합쳐 주제 `system`(정의 `io/system.json`)으로 낸다. 건강이 바뀌면 이벤트(`health_changed`, `worker_health_changed`)를 내고 record-manager가 기록한다. 누구에게도 명령하지 않는다.

## 빌드·시험

```sh
cmake -S managers/status -B ~/status-build && cmake --build ~/status-build -j && ctest --test-dir ~/status-build --output-on-failure
```

`dev/check.sh`가 이것과 TSan을 함께 돌린다. 확정(Jetson): `dev/release-worker.sh status`.

## 설정(규칙 데이터, 매니저 설정 `config["status"]`)

| 키 | 뜻 | 기본 |
|---|---|---|
| `required` | 필수 워커 id 글롭 목록. 하나라도 `fault`·`off`면 시스템 `fault` | 비면 자동 시작 워커 전부 |
| `stale_s` | heartbeat가 이만큼 없으면 `fault` | 3 |
| `metrics` | 지표 조건 `[{"worker": "cam*", "metric": "camera_fps", "below": 10, "then": "degraded"}]` | 없음 |
| `publish_ms` | 건강이 그대로일 때 주제를 다시 내는 간격(바뀌면 바로) | 1000 |

## 명령

| 명령 | 뜻 |
|---|---|
| `status.get_system` | 주제 `system`의 지금 값(읽기) |
