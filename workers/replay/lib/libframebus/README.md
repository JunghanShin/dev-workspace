# libframebus: 데이터 버스 (프레임 버스)

같은 보드의 프로세스끼리 큰 데이터(영상 프레임, 센서 배열 등)를 복사 없이 나누는 정적 라이브러리. 워크스페이스 `design.md` 6.7, 워커 간 통신은 6.6.7. 워크스페이스에 의존하지 않는다.

```text
생산자(워커 A) ── 공유 메모리 슬롯 K개(memfd) ── 소비자(워커 B, C …)
   알림 소켓(Unix SOCK_SEQPACKET): 접속 때 버퍼 fd + 형식 1회, 이후 {slot, frame_id, ts} / 반납 {release: slot}
```

| 항목 | 내용 |
|---|---|
| 형식 | 영상 `Format{width, height, pitch, 픽셀 형식}`, 원시 레코드 `Format::raw(최대 바이트, "형식 이름")`(예: `"f32x256"`) |
| 생산자 | `Producer`: `acquire()` → 슬롯에 쓰기 → `publish(slot, frame_id, ts_mono_ns[, bytes])`. 빈 슬롯이 없으면 버림(기다리지 않음). `stats()`: consumers, published, dropped, skipped |
| 소비자 | `Consumer(path, Mode::Latest)`: 최신 하나만(밀린 것은 반납). `Mode::InOrder`: 받은 순서대로 전부(`pending()`). `next(timeout_ms, &frame)` → `Frame`/`Timeout`/`Disconnected`, 쓴 뒤 `release(frame)` |
| 끊김 | 생산자가 끝나면 소비자는 `Disconnected` → 살아서 다시 `connect()` |
| 소켓 기본 위치 | 생산자 설정. 워크스페이스에서는 `<데이터>/run/frames/<워커 id>.sock` |

```cpp
// 생산자
framebus::Producer bus(path, framebus::Format::raw(256 * 4, "f32x256"), 4);
std::string err;
bus.start(&err);
if (auto s = bus.acquire()) { std::memcpy(s->data, v.data(), n); bus.publish(*s, seq, ts_ns, n); }

// 소비자(빠지면 안 되는 데이터)
framebus::Consumer c(path, framebus::Consumer::Mode::InOrder);
c.connect(&err);
framebus::Frame f;
if (c.next(100, &f) == framebus::Consumer::Result::Frame) { use(f.data, f.bytes, f.frame_id); c.release(f); }
```

`frame_id`는 생산자가 단조 증가로 붙인다. 소비자는 빈칸으로 빠진 것을 안다(본보기: 틀 `lib/peer`의 `SeqTracker`).

워커마다 `lib/libframebus/`에 사본을 둔다(원본: `workers/_template/lib/libframebus`). 같은지는 워크스페이스 `dev/test_workers.py`가 확인한다.

## 동기화와 시험

프로세스 사이 락이 없다. 공유 메모리에는 데이터만 있고, 슬롯 상태(쓰는 중 / 읽는 소비자 수)는 생산자 안의 장부 한 곳(mutex)에 있다. 슬롯은 "생산자 혼자 씀" 또는 "소비자들이 읽기만" 둘 중 하나이고, 넘김은 소켓 메시지(publish → release)로 한다. 소비자는 읽기 전용으로 매핑하고, 끊기면 생산자가 그 소비자 몫을 회수한다(워크스페이스 design.md 6.7).

- `test/test_framebus.cpp`: 전달, 최신 우선, 빈 슬롯 없을 때 버림, 끊김 시 참조 해제, 생산자 종료, 원시 레코드·InOrder
- `test/test_framebus_stress.cpp`: 생산자 하나 + 소비자 넷 동시. 들고 있는 동안 내용 두 번 검사(덮어쓰기 감지), 번호 순서, 반납 없이 끊기, 끝나면 슬롯 전부 회수. 단계 ① 빠른 소비자 ② 느린 InOrder
- ThreadSanitizer: 워크스페이스 `dev/check.sh`가 이 폴더를 TSan으로 빌드해 두 시험을 돌린다(경고 0)
