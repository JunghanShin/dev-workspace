# @NAME@ 워커 (`@TYPE@`)

무엇을 하는 워커인지 한두 줄.

## 구성 (워커 템플릿, 워크스페이스 design.md 6.6.3·6.6.4)

| 층 | 폴더 | 워크스페이스 밖에서 |
|---|---|---|
| ① 기능 코어(유닛) | `lib/<모듈>/core/` | 이것만 다른 프로그램에 복사해 써도 됨(libworker 의존 없음) |
| ② 워커 실행 | `lib/<모듈>/include·src`(모듈), `lib/libworker`, `src/main.cpp` | 독립 실행 파일 `@NAME@-worker --config <파일>` |
| ③ 워크스페이스 연동 | `workspace/`(type·스키마·prepare·install·확정 실행 파일·패널), libworker 옵션 `WORKER_WORKSPACE` | `workspace/` 삭제, `-DWORKER_WORKSPACE=OFF`로 빌드 |

## 설정 (`config.<모듈>`, 프리셋 `workspace/presets/`)

| 키 | 기본값 | 내용 |
|---|---|---|
| | | |

맨 위에는 호스트 키(`watchdog_sec`, `stop_timeout_sec`, `thread_stall_sec`, `command_socket`, `stream_socket`)만.

## 명령 (`<모듈>.<명령>`)

| cmd | args | 응답 |
|---|---|---|
| | | |

## 지표 (`<모듈>_<이름>`)

## 스레드 (`<모듈>.<역할>`)

## 다른 워커와의 연결 (설정 칸 `worker_id`, 인터페이스 명세는 design.md 3장)

## 빌드·시험

```bash
jetson-build --native --test
```

```bash
jetson-build --test
```

확정(Jetson 실행 파일을 저장소에): 소스를 커밋한 뒤 워크스페이스 폴더에서. Jetson 빌드·시험(`jetson-build --test`)도 이 스크립트가 직접 한다

```bash
dev/release-worker.sh @NAME@
```

## 워크스페이스 밖에서 쓰기

```bash
rm -rf workspace && cmake -S . -B build/standalone -DWORKER_WORKSPACE=OFF && cmake --build build/standalone
```

설정 파일 예: `{"id": "@NAME@", "config": {"source": {}, "processor": {}, "peer": {}}}`
