# config-manager

설정·버전 매니저(기본 탑재). 설계는 `design.md`, 매니저 공통 틀은 `../../docs/managers.md`.

설정을 쓰는 곳(워커 설정은 process-manager, 워크스페이스 설정은 파일, 사용자·권한은 게이트웨이)은 그대로 두고 위에서 본다: 실행 파일 BUILD와 libworker로 소프트웨어 버전 목록(실제 파일 sha256이 BUILD와 다르면 이벤트), 설정 묶음(스냅숏: 워커 설정·연결 선언·워크스페이스 설정 파일·버전) 남기기·비교·되돌리기. 되돌리기는 워커 설정·연결 선언만, 개발 모드에서만(process-manager 쓰기 소켓, `manager_api`). 주제 `config`(정의 `io/config.json`).

## 빌드·시험

```sh
cmake -S managers/config -B ~/config-build && cmake --build ~/config-build -j && ctest --test-dir ~/config-build --output-on-failure
```

`dev/check.sh`가 이것과 TSan을 함께 돌린다. 확정(Jetson): `dev/release-worker.sh config`.

## 설정(`config["config"]`)

| 키 | 뜻 | 기본 |
|---|---|---|
| `keep` | 스냅숏 남길 수(넘으면 오래된 auto부터 지움, 이름 붙인 것은 남김) | 20 |
| `root`, `data` | 워크스페이스 루트·데이터 폴더(시험용) | `$WORKSPACE_ROOT`, `$WORKSPACE_DATA` |

스냅숏 파일: `<데이터>/config/snapshots/<시각>.json`.

## 명령

| 명령 | 뜻 |
|---|---|
| `config.versions` | 버전 목록·어긋남·libworker·설치 기록(읽기) |
| `config.snapshots` | 남긴 스냅숏(읽기) |
| `config.diff` | `{"id"}` 그 스냅숏과 지금의 다른 점(읽기) |
| `config.snapshot` | `{"label"}` 지금 것을 남김(운용 조작) |
| `config.restore` | `{"id", "workers"?}` 워커 설정·연결 선언을 되돌림(개발 모드) |
