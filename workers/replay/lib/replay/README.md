# replay 모듈 (`lib/replay`)

데이터 버스(libframebus) 녹화·재생. 워커 설계는 `../../design.md`.

| 층 | 폴더 | 내용 |
|---|---|---|
| ① 유닛(코어) | `core/` | `replay/recording.hpp`: 녹화 파일 쓰기(`Writer`)·읽기(`Reader`)·목록·삭제. libworker·libframebus 모름 |
| ② 모듈 | `include/`, `src/` | `replay::Module`: 설정 `replay`, 명령 `replay.*`, 스레드 `replay.rec`·`replay.play` |
| 시험 | `test/` | `test_recording`(유닛), `test_module`(호스트 없이 설정·인자) |
| 설정 칸 원본 | `schema.json` | 워커 `workspace/worker-types.json`에 `"section": "replay"`로 옮김 |

## 녹화 파일

`<녹화 폴더>/<이름>/`

- `meta.json`: `width`, `height`, `stride`, `slot_bytes`, `pixel`, `source`, `created`, `frames`, `bytes`, `duration_ns`
- `frames.bin`: 머리 `"FBREC1\n\0"`(8바이트), 이어서 레코드마다 머리 24바이트 `{u64 frame_id, i64 ts_ns, u32 bytes, u32 0}` + 데이터

레코드 크기는 프레임마다 다를 수 있다. 끝이 잘린 파일(전원 꺼짐)은 온전한 레코드까지만 읽는다.
이름은 `a-z A-Z 0-9 - _`, 1~64자.
