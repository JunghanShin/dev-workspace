# payload-manager

모듈 운용 매니저(기본 탑재). 설계는 `design.md`, 매니저 공통 틀은 `../../docs/managers.md`.

시스템에 들어간 모듈(매니저가 아닌 워커)의 목록을 들고(역할은 계약, 상태는 process-manager, 건강은 status-manager 주제 `system`), 모듈을 켜고 끈다(process-manager `start`·`stop` 요청, 운용 모드에서도 됨). 매니저는 건드리지 않는다. 주제 `payload`(정의 `io/payload.json`).

## 빌드·시험

```sh
cmake -S managers/payload -B ~/payload-build && cmake --build ~/payload-build -j && ctest --test-dir ~/payload-build --output-on-failure
```

`dev/check.sh`가 이것과 TSan을 함께 돌린다. 확정(Jetson): `dev/release-worker.sh payload`.

## 설정(`config["payload"]`)

| 키 | 뜻 |
|---|---|
| `groups` | 묶음 `{"<이름>": ["<워커 id 글롭>", ...]}`. `payload.power_group`으로 한꺼번에 켜고 끔 |

## 명령

| 명령 | 뜻 |
|---|---|
| `payload.list` | 모듈 목록(읽기) |
| `payload.power` | `{"module", "on"}` → process-manager `start`/`stop` 요청, 답에 `desired_state`(운용 조작) |
| `payload.power_group` | `{"group", "on"}` → 묶음의 모듈마다, 모듈별 결과(운용 조작) |
