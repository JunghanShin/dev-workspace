# replay 워커 (`replay_worker`)

다른 워커의 데이터 버스(공유 메모리 프레임)를 녹화하고, 그 녹화를 원래 시간 간격대로 다시 내보낸다.
장치 없이 같은 입력으로 처리 워커를 되풀이 시험하거나, 현장에서 받은 데이터를 사무실에서 재현할 때 쓴다.
개발 도구다(제품 묶음에는 넣지 않아도 된다). 설계는 `design.md`.

## 구성 (워커 템플릿, 워크스페이스 design.md 6.6.3·6.6.4)

| 층 | 폴더 | 워크스페이스 밖에서 |
|---|---|---|
| ① 기능 코어(유닛) | `lib/replay/core/` | 녹화 파일 읽기·쓰기만. 다른 프로그램에 복사해 써도 됨(libworker 의존 없음) |
| ② 워커 실행 | `lib/replay/include·src`(모듈), `lib/libworker`, `lib/libframebus`, `src/main.cpp` | 독립 실행 파일 `replay-worker --config <파일>` |
| ③ 워크스페이스 연동 | `workspace/`(type·스키마·prepare·install·확정 실행 파일·패널), libworker 옵션 `WORKER_WORKSPACE` | `workspace/` 삭제, `-DWORKER_WORKSPACE=OFF`로 빌드 |

## 쓰는 법

1. 워크스페이스에 replay 워커를 하나 만들고(id 예: `replay`) 시작
2. 패널 "녹화·재생"에서 받을 워커 id(예: `cam1`)를 넣고 녹화 → 멈춤
3. 받는 워커(처리 워커)의 설정 상대 워커(peer)를 `replay`로 바꾸고, 녹화 목록에서 ▶(배속·반복 선택)

명령으로:

```bash
curl -s -X POST localhost:8080/api/board/workers/replay/command -H 'Content-Type: application/json' \
  -d '{"cmd":"replay.record_start","args":{"source":"cam1","name":"run1","seconds":30}}'
```

## 설정 (`config.replay`, 프리셋 `workspace/presets/`)

| 키 | 기본값 | 내용 |
|---|---|---|
| `dir` | `<데이터>/records/framebus` | 녹화 폴더(워크스페이스 밖이면 `$XDG_RUNTIME_DIR/<id>-records`) |
| `max_total_mb` | 4096 | 녹화 폴더 전체 상한. 넘으면 녹화를 멈춤 |
| `max_record_sec` | 600 | 녹화 하나 최대 길이 |
| `bus_slots` | 8 | 재생 데이터 버스 슬롯 수 |
| `bus_socket` | `default_socket(id, "frames")` | 재생 데이터 버스 소켓 |

맨 위에는 호스트 키(`watchdog_sec`, `stop_timeout_sec`, `thread_stall_sec`, `command_socket`, `stream_socket`)만.

## 명령 (`replay.<명령>`)

| cmd | args | 응답 |
|---|---|---|
| `replay.record_start` | `source`(워커 id) 또는 `socket`, `name`, `seconds`? | `{recording, socket, seconds}` |
| `replay.record_stop` | — | `{stopped, frames}` |
| `replay.play_start` | `name`, `speed`=1(0~100], `loop`=false | `{playing, frames, speed, loop, socket}` |
| `replay.play_stop` | — | `{stopped}` |
| `replay.list` | — | `{recordings: [{name, source, created, frames, seconds, pixel, width, height, mb}], total_mb, max_total_mb, dir}` |
| `replay.delete` | `name` | `{deleted}`. 녹화·재생 중이면 `busy` |

## 지표 (`replay_<이름>`)

`recording`, `rec_frames`, `playing`, `play_frames`(녹화 상대 연결은 상태 `recording.link`: 녹화 중에만 뜻이 있어 `_link` 지표로 내지 않음)

## 스레드 (`replay.<역할>`)

`replay.rec`(녹화: 받기 → 파일), `replay.play`(재생: 파일 → 데이터 버스). 둘 다 워커가 시작할 때 만들고 일이 없으면 쉰다.

## 다른 워커와의 연결

- 녹화: 명령의 `source` 워커의 데이터 버스를 받는 쪽(InOrder)으로. 생산자가 없으면 기다리며 다시 붙음
- 재생: 이 워커의 데이터 버스로 냄. 받는 워커가 설정 `peer`(worker_id)를 이 워커 id로

## 빌드·시험

```bash
jetson-build --native --test
```

```bash
jetson-build --test
```

확정(Jetson 실행 파일을 저장소에): 소스를 커밋한 뒤 워크스페이스 폴더에서. Jetson 빌드·시험(`jetson-build --test`)도 이 스크립트가 직접 한다

```bash
dev/release-worker.sh replay
```

## 워크스페이스 밖에서 쓰기

```bash
rm -rf workspace && cmake -S . -B build/standalone -DWORKER_WORKSPACE=OFF && cmake --build build/standalone
```

설정 파일 예: `{"id": "replay", "config": {"replay": {"dir": "/data/records"}}}`
