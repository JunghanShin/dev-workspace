# fault-manager

이상 판단 매니저(기본 탑재). 설계는 `design.md`, 매니저 공통 틀은 `../../docs/managers.md`.

규칙(조건 → 심각도 → 대응)으로 이상을 올리고 내린다. 조건은 워커 건강(status 주제 `system`), heartbeat 지표, 이벤트, 시스템 건강. 이상을 올릴 때 규칙의 대응(`do`)을 mission-manager에 `mission.send`로 한 번 요청한다(운용 모드에서만, 개발 모드에서는 "할 뻔한 대응"만 남김). **기본 규칙은 없다**(플랫폼은 장비를 모름). 주제 `fault`(정의 `io/fault.json`).

## 빌드·시험

```sh
cmake -S managers/fault -B ~/fault-build && cmake --build ~/fault-build -j && ctest --test-dir ~/fault-build --output-on-failure
```

`dev/check.sh`가 이것과 TSan을 함께 돌린다. 확정(Jetson): `dev/release-worker.sh fault`.

## 규칙(`config["fault"]["rules"]`, 프로젝트 프로필로)

```json
[{"id": "cam_lost", "when": {"health": {"worker": "cam*", "is": ["fault", "off"]}}, "for_s": 3, "severity": "error",
  "do": [{"target": "payload-manager", "cmd": "payload.power", "args": {"module": "cam1", "on": true}}]}]
```

조건 `health` · `metric`(`name`, `above`/`below`) · `event`(`code` 글롭, `hold_s`) · `system`(`is`). 자세한 것은 `design.md` 2장.

## 명령

| 명령 | 뜻 |
|---|---|
| `fault.list` | 올라간 이상(읽기) |
| `fault.ack` | `{"id"}` 사람이 봤다는 표시(운용 조작) |
