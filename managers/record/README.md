# record-manager

기록 매니저(기본 탑재). 설계는 `design.md`, 매니저 공통 틀은 `../../docs/managers.md`.

process-manager 상태(워커·시스템·명령)와 워커·매니저의 주제·이벤트를 같은 시간축으로 `<데이터>/records/*.jsonl`에 남긴다. 예전 Python `workspace-logger`(지움)와 같은 파일·같은 줄 모양이라 게이트웨이 `/history`가 그대로 읽는다.

## 빌드·시험

```sh
cmake -S managers/record -B ~/rec-build && cmake --build ~/rec-build -j && ctest --test-dir ~/rec-build --output-on-failure
RECORD_BIN=~/rec-build/record-manager python3 -m unittest managers/record/test_compare.py   # 예전 logger 정답 줄과 비교
```

`dev/check.sh`가 위 둘과 TSan을 함께 돌린다.

## 단독 실행

```sh
cat > /tmp/rec.json <<'JSON'
{"id": "record-manager", "config": {"record": {"dir": "/tmp/records", "manager_socket": "<데이터>/run/manager-logger.sock"}}}
JSON
~/rec-build/record-manager --config /tmp/rec.json
```

설정 키는 `design.md` 2.2(`dir`, `manager_socket`, `interval_s`, `max_file_mb`, `max_total_mb`, `fsync_s`, `topics`, `exclude`). `dir`·`manager_socket`를 비우면 `WORKSPACE_DATA` 기준 기본 경로.

## 명령

| 명령 | 뜻 |
|---|---|
| `record.find` | 시간 범위·종류·보낸 곳으로 찾기(읽기). 인자는 `design.md` 2.4 |
