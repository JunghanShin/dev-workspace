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

조합·단계(`escalate.md`): `when`에 `{"all": [...]}`·`{"any": [...]}`(중첩 4단까지, 시스템 전체로 한 번 판정, 이상 이름 = 규칙 id), `stages: [{"after_s", "severity", "text"?, "do"?}]`(올라간 뒤 시간이 지나면 심각도를 올리고 그 단계 대응을 한 번, 이벤트 `fault_escalated`).

```json
{"id": "cam_hot_lost", "when": {"all": [{"health": {"worker": "cam*", "is": "fault"}}, {"metric": {"worker": "system-monitor", "name": "temp_max_c", "above": 80}}]},
 "for_s": 2, "severity": "warning", "do": [{"target": "payload-manager", "cmd": "payload.restart", "args": {"module": "cam1"}}],
 "stages": [{"after_s": 30, "severity": "error", "do": [{"target": "payload-manager", "cmd": "payload.stop", "args": {"module": "cam1"}}]},
            {"after_s": 120, "severity": "critical"}]}
```

## 명령

| 명령 | 뜻 |
|---|---|
| `fault.list` | 올라간 이상(읽기) |
| `fault.ack` | `{"id"}` 사람이 봤다는 표시(운용 조작) |
