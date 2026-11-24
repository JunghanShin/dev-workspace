# mission-manager

명령 송수신 매니저(기본 탑재). 설계는 `design.md`, 매니저 공통 틀은 `../../docs/managers.md`.

게이트웨이는 읽기가 아닌 워커 명령을 이 매니저로 넘기고(`mission.send`), 이 매니저가 (대상 워커, 역할)마다 한 번에 하나씩 워커로 보낸다. 같은 역할에 동시에 온 명령은 차례로, 대기가 차면 `busy`. 워커의 `last_command`에는 `from: mission-manager`, `origin: <처음 보낸 곳>`이 남는다. 이 매니저가 설치돼 있지 않으면 게이트웨이는 지금처럼 워커로 바로 보낸다.

## 빌드·시험

```sh
cmake -S managers/mission -B ~/mission-build && cmake --build ~/mission-build -j && ctest --test-dir ~/mission-build --output-on-failure
```

`dev/check.sh`가 이것과 TSan을 함께 돌린다. 확정(Jetson): `dev/release-worker.sh mission`.

## 설정(`config["mission"]`)

| 키 | 뜻 | 기본 |
|---|---|---|
| `queue` | (대상, 역할)마다 대기 상한. 넘치면 `busy` | 4 |
| `command_timeout_ms` | 명령 하나의 응답 대기 | 2000 |
| `total_timeout_ms` | 큐에서 기다린 시간 포함 상한(게이트웨이 3 s보다 짧게) | 2800 |
| `idle_close_s` | 쓰지 않는 명령 스레드를 닫기까지 | 60 |

## 명령

| 명령 | 뜻 |
|---|---|
| `mission.send` | `{"target", "cmd", "args"}` → 대상 워커의 응답 그대로(운용 조작) |
| `mission.get_queues` | 지금 대기 큐(읽기) |
