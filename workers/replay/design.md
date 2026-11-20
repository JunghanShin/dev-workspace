# replay 워커 설계

| 항목 | 내용 |
|---|---|
| 문서 버전 | 0.1 |
| type | `replay_worker` |
| 실행 파일 | `replay-worker` (C++17, CMake). 이 폴더만으로 빌드 |
| 단계 | 워커(CSCI). 워크스페이스 design.md 3장 "단계". 개발 도구(제품 묶음 선택) |
| 모듈 | `replay`(lib/replay) |
| 상태 | 구현, PC 시험(녹화 → 재생 왕복) 통과 |

## 1. 목적

데이터 버스(libframebus) 녹화·재생. 다른 워커가 내는 프레임을 순서대로 전부 파일에 저장하고, 그 파일을 원래 시간 간격(배속)대로
이 워커의 데이터 버스로 다시 낸다. 받는 워커는 상대 워커 id만 바꾸면 실제 장치 대신 녹화를 받으므로

- 장치 없이 같은 입력으로 처리 워커를 되풀이 시험(회귀 시험, 성능 비교)
- 현장 데이터를 사무실에서 재현(문제 재현)

에 쓴다. 가짜 장치 모드(워크스페이스 design.md 6.6.8)는 장치 프로토콜을 흉내 내고, 이 워커는 장치 뒤 데이터를 되풀이한다.

## 2. 구조

### 2.1 모듈과 유닛

| 모듈(CSC) | 하는 일 | 유닛(CSU, 코어) | 포트(내주는 것 / 받는 것) |
|---|---|---|---|
| `replay` | 설정·명령, 녹화·재생 스레드, 데이터 버스 받기·내기 | `recording`: 녹화 파일 `Writer`·`Reader`·`list`·`remove` | 없음(모듈 하나) |

- 코어는 파일 형식만 안다(libworker·libframebus 모름). 모듈이 데이터 버스와 파일을 잇는다
- 파일 형식: `lib/replay/README.md`

### 2.2 실행 구조

| 스레드·프로세스 | 하는 일 | 대기 상한 | 사이 큐(크기, 넘칠 때) |
|---|---|---|---|
| (호스트 루프) | 명령, 상태. 녹화·재생 일(Job)을 스레드에 넘김 | 멈춤 명령은 스레드가 끝날 때까지 최대 1초 | 일 하나(진행 중이면 `busy`) |
| `replay.rec` | 받기(InOrder) → 파일 쓰기 | 받기 100 ms, 접속 재시도 200 ms | 데이터 버스 슬롯(생산자 쪽). 느리면 생산자가 버림 |
| `replay.play` | 파일 읽기 → 원래 간격 대기 → 데이터 버스 | 대기 중 50 ms마다 beat, 빈 슬롯 40 ms | 데이터 버스 슬롯. 빈 슬롯이 없으면 그 프레임 버림(생산자 규칙) |

두 스레드는 워커가 시작할 때 만들어 끝날 때까지 돈다(스레드 등록은 시작 때만, libworker ThreadWatch 규칙).

## 3. 인터페이스 명세 (ICD)

**칸 목록은 정의 파일이 원본이다**(워크스페이스 design.md 6.6.7.1): 역할 정의 `io/recorder.json`, 워커 정의 `io/self.json`(`get_status` 칸·지표), 공통 정의(틀 `io/common.json`). 실제 출력이 정의와 맞는지는 출력 대조(`io.json` `check`)가 본다. 아래 표는 뜻·시점만.

오류 code는 워크스페이스 9.6 목록만 쓴다(예전 `conflict` → 진행 중이면 `busy`, 같은 이름은 `bad_request`).

### 3.1 명령 (명령 소켓, `replay.<명령>`)

| cmd | args | 응답 data | 오류 code | 바로 적용 / 저장 |
|---|---|---|---|---|
| `get_status` | — | 3.2 | — | — |
| `replay.record_start` | `source`(워커 id) 또는 `socket`, `name`, `seconds`? | `{recording, socket, seconds}` | `bad_request`(이름·인자, 같은 이름), `busy`(녹화 중), `unavailable`(폴더 가득) | 파일 저장 |
| `replay.record_stop` | — | `{stopped, frames}` | — | — |
| `replay.play_start` | `name`, `speed`=1 (0,100], `loop`=false | `{playing, frames, speed, loop, socket}` | `bad_request`(녹화 없음, 배속), `busy`(재생 중), `unavailable`(데이터 버스) | — |
| `replay.play_stop` | — | `{stopped}` | — | — |
| `replay.list` | — | `{recordings[], total_mb, max_total_mb, dir}` | — | — |
| `replay.delete` | `name` | `{deleted}` | `busy`(사용 중), `bad_request`(없음) | 파일 삭제 |

### 3.2 상태 (`get_status`의 `replay` 칸)

| 칸 | 형식 | 뜻 |
|---|---|---|
| `replay.dir`, `replay.bus_socket` | 글자 | 녹화 폴더, 재생 소켓 |
| `replay.recording` | `{active, name, source, link, frames, mb, error}` | 녹화(끝나도 마지막 값 유지) |
| `replay.playing` | `{active, name, frames, total, loops, speed, loop, consumers, error}` | 재생 |

### 3.3 지표 (heartbeat, `replay_<이름>`, 1초)

| 지표 | 단위 | 뜻 |
|---|---|---|
| `replay_recording` | 0/1 | 녹화 중 |
| `replay_rec_frames` | 개 | 이번 녹화 프레임 |
| `replay_playing` | 0/1 | 재생 중 |
| `replay_play_frames` | 개 | 이번 재생에서 낸 프레임 |

### 3.4 스트림 주제

없음.

### 3.5 데이터 버스

| 방향 | 소켓 | 형식(`Format`) | 레코드 | 받는 쪽 모드 |
|---|---|---|---|---|
| 받음(녹화) | 명령 `source`의 `default_socket(<id>, "frames")` 또는 `socket` | 상대 형식 그대로 | 상대 레코드 그대로 | InOrder(빠짐 없이) |
| 내보냄(재생) | 설정 `bus_socket`(기본 `<데이터>/run/frames/<id>.sock`) | 녹화 `meta.json`의 형식 | 녹화 레코드(frame_id 그대로, 시각은 재생 시각) | 받는 쪽이 고름 |

### 3.6 다른 워커와의 연결

| 상대 | 통로 | 주고받는 것 | 상대가 없을 때 |
|---|---|---|---|
| 녹화 대상(명령 `source`) | 데이터 버스 | 프레임 받기 | 상태 `recording.link=false`, 기다리며 다시 붙음(녹화 시간은 흐름). `_link` 지표로 내지 않음(녹화 중에만 뜻이 있어, 쉬는 동안 끊김 경고가 되지 않게) |
| 재생 받는 워커(그 워커의 설정 peer = 이 id) | 데이터 버스 | 프레임 내기 | 받는 쪽 없이도 재생(프레임은 슬롯만 돌다 사라짐) |

### 3.7 설정 (원본: `lib/replay/schema.json`)

| 키 | 형식 | 기본값 | 바로 적용(live) | 뜻 |
|---|---|---|---|---|
| `replay.dir` | 글자 | `<데이터>/records/framebus` | 아니오 | 녹화 폴더 |
| `replay.max_total_mb` | 수 | 4096 | 아니오 | 폴더 전체 상한 |
| `replay.max_record_sec` | 수 | 600 | 아니오 | 녹화 하나 최대 길이 |
| `replay.bus_slots` | 정수 2~64 | 8 | 아니오 | 재생 슬롯 수 |
| `replay.bus_socket` | 글자 | 기본 위치 | 아니오 | 재생 소켓 |

### 3.8 가짜 장치

해당 없음(장치를 쓰지 않음). 이 워커 자체가 장치 없는 개발을 돕는다.

## 4. 장애 처리

- 녹화 상대가 없거나 끊김: 종료 사유 아님(상태 `recording.link=false`, 다시 붙음)
- 디스크 쓰기 실패·폴더 상한: 녹화를 멈추고 `recording.error`에 적음. 파일은 그때까지 온전함(meta 갱신)
- 전원 꺼짐으로 잘린 파일: 온전한 레코드까지만 재생
- 스레드 예외만 `failed()` → exit 4 → Manager가 다시 띄움

## 5. 시험

| 단계 | 시험 | 내용 |
|---|---|---|
| 유닛 | `lib/replay/test/test_recording.cpp` | 쓰기 → 읽기(크기 바뀌는 레코드), 같은 이름 실패, 잘린 파일, 목록·삭제, 이름 검사 |
| 모듈 | `lib/replay/test/test_module.cpp` | 설정 읽기·검사, 명령 인자 검사, 시작 전 상태 |
| 워커 | `test/test_worker.cpp` | 가짜 생산자 녹화 → 목록 → 2배속 재생을 받는 쪽이 같은 번호·내용·순서로 전부 받음 → 삭제, SIGTERM |
| 시스템 | 보드 `dev/system-check.sh` | 실기: 실행·heartbeat |

## 6. 미결

- 여러 버스 동시 녹화(지금은 하나씩)
- 스트림(텔레메트리) 녹화
