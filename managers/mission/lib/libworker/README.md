# libworker: C/C++ 워커 공통 뼈대

워크스페이스 워커 계약(`design.md` 6.6, 9장)을 지키는 데 필요한 부분만 모은 정적 라이브러리. **워크스페이스에 의존하지 않는다.** Manager가 있으면 heartbeat를 보내고, 없으면(단독 실행) 보내지 않는 것 말고는 같은 코드로 돈다.

| 헤더 | 내용 |
|---|---|
| `worker/workspace.hpp` | **워크스페이스 연동(③층)이 닿는 유일한 곳**: Manager heartbeat(`Notifier`), 소켓 기본 경로(`default_command_socket`/`default_stream_socket`), `workspace_data_dir`, `WORKSPACE_WORKER_ID`. 빌드 옵션 `WORKER_WORKSPACE`(기본 ON: `src/workspace/workspace.cpp`, OFF: `src/standalone.cpp` — heartbeat 없음, 소켓은 `$XDG_RUNTIME_DIR`·`/tmp`) |
| `worker/host.hpp` | **호스트와 모듈**(모듈은 `host()`로 붙은 호스트를 앎, `thread_stall_default`)(design.md 6.6.3): `Host`가 실행 인자·설정(모듈 칸 나눠 주기)·워치독·스레드 감시·heartbeat·명령 소켓(배달, `get_status` 모으기)·종료를 맡고, `Module`(이름, `configure`, `reconfigure`(바로 적용, SIGHUP → `reload_file`), `start`, `add_pollfds`/`on_poll`/`on_timer`/`next_wakeup`, `handle`, `status`, `metrics`, `failed`, `stop`)을 붙인다. `Metrics`(지표 접두어), `alias`(예전 명령 이름), `legacy_keys`(예전 설정), `call_module`(모듈 시험), `libworker_version()`(READY의 `libworker=`, 버전은 CMakeLists `project(... VERSION)`. 지금 0.9.1). **기본 안전망**(항상 켜짐, design.md 6.6.9 ②): 모든 진입점 예외 잡기(명령은 오류 응답, `on_timer`·`on_poll` 연달아 3번이면 exit 4, `worker::Fatal`은 바로), 루프 시간 지표 `host_loop_ms`, 멈춘 스레드가 이어지면 워치독보다 먼저 `safe_state()` 뒤 exit 4, `get_status`의 `runtime`. 호스트 키 `stages`, `loop_budget_ms` |
| `worker/config.hpp` | 설정 파일 읽기(`{"id", "config_version", "config"}`, id 없으면 `WORKSPACE_WORKER_ID` → 파일 이름), 키·타입 검사, 명령 소켓 기본 경로 |
| `worker/command_server.hpp` | 명령 소켓 서버(9.6). 논블로킹, 단일 스레드 이벤트 루프용 |
| `worker/client.hpp` | 소켓 클라이언트: `send_command`(명령 소켓, CLI·시험용), `send_request`(JSON 한 줄 요청·응답), `manager_call`(워커 선언 `"manager_api": true`인 워커만: Manager API, 환경 변수 `WORKSPACE_MANAGER_SOCKET`. 아니면 `no_manager_api`, design.md 6.5) |
| `worker/peer.hpp` | **워커 간 통신**(design.md 6.6.7): `Subscriber`(상대 스트림 구독, 끊기면 다시 붙음, `next()` → `Update{down, kf, topics, events}`), `CommandClient`(상대 명령 소켓으로 비동기 요청, `send()` → 번호, `next()` → `Reply`). 둘 다 이름 있는 스레드 + 큐, `wake_fd()`를 `add_pollfds`에. 큰 데이터는 libframebus `Consumer` |
| `worker/runtime.hpp` | heartbeat(`WORKSPACE_NOTIFY_SOCKET`), 자체 워치독(커널 타이머), SIGTERM·SIGINT 정지 요청 |
| `worker/child.hpp` | 자식 프로세스(ffmpeg 등) 띄우기·끝내기 |
| 가짜 장치 모드(`host.hpp`) | 설정 맨 위 `"sim": {"cmd": "@EXEC_DIR@/<이름>-sim ..."}`(또는 `{}`): 호스트가 가짜 장치 프로그램을 모듈보다 먼저 자식으로 띄우고 죽으면 2 s 뒤 다시, 끝날 때 정리. READY `sim=1`, `get_status`의 `sim`, `Host::sim()`. `exec_dir()`·`expand_exec_dir()`(실행 파일 폴더, `@EXEC_DIR@`) (design.md 6.6.8) |
| `worker/stage.hpp` | **제공 도구: 입력·출력 단계**(design.md 6.6.9 ③): `host.input<T>(이름, 읽기, 처리)`, `host.output<T>(이름, 쓰기)`. 스레드 + 큐 + 대기 상한 + 멈춤 감지 + 지표(`<단계>_q`, `_q_max`, `_lag_ms`, `_drops`, `_errors`)를 한 번에, 읽기·쓰기 예외는 세고 계속, 처리 예외는 그 항목만 버림, 처리 시간 예산. 설정 `stages`로 크기·넘칠 때·대기 상한·우선순위·CPU. `worker::Fatal` |
| `worker/mutex.hpp` | 제공 도구: 우선순위 상속 잠금 `worker::Mutex`(실시간 스레드와 같이 쓰는 잠금) |
| `worker/queue.hpp` | 스레드 사이 큐 `BoundedQueue<T>`: 용량 제한, 넘칠 때 정책(`DropOldest`/`Reject`/`DropNewest`), push는 막히지 않음, `pop_for`(시간 제한) 또는 `wake_fd()`를 `poll()`에 넣어 대기, `close()`, 통계 |
| `worker/thread.hpp` | `WorkerThread`(이름 있는 스레드. 이름은 운영체제 스레드 이름으로도 붙음(15자, 규칙 `<모듈>.<역할>`), 정지 요청, `beat()`, 예외 → `failed()`), `ThreadWatch`(스레드 멈춤 감지, `all_alive()`일 때만 워치독 갱신. 실행 중 언제 등록해도 안전, 할 일을 다 하고 끝난 스레드는 검사에서 빠짐). 제공 도구: `ThreadOptions`(실시간·nice·CPU 고정)를 `set_options` 또는 `apply_thread_options`로. 권한이 없으면 경고하고 보통으로 계속, `info()`에 적용 결과 |
| `third_party/nlohmann/json.hpp` | JSON (v3.11.3, MIT) |
| `tools/run-workers.sh` | 워커 여러 개를 워크스페이스 없이 함께 띄우고 함께 내리는 셸 |

## 워커 내부 구조 (병렬 파이프라인, design.md 6.6.1)

통신·장치·영상 워커는 RX 스레드 → rx 큐 → Core(상태 단일 소유) → tx 큐 → TX 스레드로 만든다.

```cpp
worker::ThreadWatch watch(std::chrono::milliseconds(1500));      // 이보다 오래 beat가 없으면 멈춘 것
worker::BoundedQueue<Message> rx_q(1024, worker::Overflow::DropOldest);
worker::BoundedQueue<Message> tx_q(256, worker::Overflow::Reject);

worker::WorkerThread rx("rx", watch, [&](worker::WorkerThread& t) {
    while (!t.stopping()) {
        if (auto m = link.receive(100ms)) rx_q.push(std::move(*m));  // 시간 제한 있는 대기
        t.beat();
    }
});
worker::WorkerThread tx("tx", watch, [&](worker::WorkerThread& t) {
    while (!t.stopping()) {
        if (auto m = tx_q.pop_for(100ms)) link.send(*m);
        t.beat();
    }
});
rx.start();
tx.start();

// Core: poll(rx_q.wake_fd(), 명령 소켓, 타이머) → rx_q.drain() → 상태·명령 처리
// 1초마다: if (watch.all_alive(now)) { worker::arm_watchdog(...); notify.send("WATCHDOG=1 ..."); }
```

## 다른 워커와 주고받기 (design.md 6.6.7)

```cpp
// 모듈 start(): 상대 id는 설정 칸(스키마 "format": "worker_id")에서
sub_ = std::make_unique<worker::Subscriber>(host.watch(), cfg_.peer, worker::Subscriber::Options{"peer.sub"});
cmd_ = std::make_unique<worker::CommandClient>(host.watch(), cfg_.peer, worker::CommandClient::Options{"peer.cmd"});
sub_->start();
cmd_->start();
// add_pollfds: fds.push_back({sub_->wake_fd(), POLLIN, 0}); fds.push_back({cmd_->wake_fd(), POLLIN, 0});
// on_poll:     while (auto u = sub_->next()) { if (!u->down) last_ = u->topics; }
//              while (auto r = cmd_->next()) { /* r->id로 대기 중 요청을 찾아 reply(r->response) */ }
// 보내기:      uint64_t id = cmd_->send("source.set_rate", {{"hz", 20}});   // 0이면 큐가 참
//              cmd_->send(cmd, args, {{"origin", "screen"}});   // 봉투에 칸 더하기(0.14, 명령 입구 매니저)
```

상대가 없거나 다시 시작해도 살아서 다시 붙는다(`connected()`, `reachable()` → 지표 `<모듈>_link`). 본보기: 틀의 `lib/peer`.

## 매니저 도구 (`worker/manager_link.hpp`, 0.13, 워크스페이스 docs/managers.md)

```cpp
// 여러 워커 입력(io.json uses "many": true): 입력 파일의 "many" 칸. 1 s쯤마다 refresh()
worker::ManyInputs many("", host.id());          // 경로를 비우면 WORKSPACE_INPUTS_FILE
if (many.refresh()) for (const auto& id : many.workers("streams")) { /* 구독 더하고 빼기 */ }
// process-manager 구독(Manager API subscribe). 끊기면 다시 붙음. 받은 줄은 키 순서를 지킨 JSON
worker::ManagerSubscription::Options o;          // socket을 비우면 WORKSPACE_MANAGER_SOCKET
o.topics = {"workers", "system"};
pm_ = std::make_unique<worker::ManagerSubscription>(host.watch(), o);
pm_->start();
// add_pollfds: fds.push_back({pm_->wake_fd(), POLLIN, 0});
// on_poll:     while (auto e = pm_->next()) { /* e->kind: Connected | Line(e->msg) | Down(e->reason) */ }
```

본보기: `managers/record`, `managers/status`.

시험: 이 폴더에서 `jetson-build --test`(또는 `--native --test`).

명령 소켓 기본 경로(설정 `command_socket`이 없을 때): `$WORKSPACE_DATA/run/cmd/<id>.sock` → `$XDG_RUNTIME_DIR/<id>.sock` → `/tmp/<id>.sock`.

워커마다 자기 폴더 `lib/libworker/`에 이 폴더의 사본을 둔다(원본 틀: `workers/_template/lib/libworker`). 워커 폴더 `CMakeLists.txt`에서:

```cmake
add_subdirectory(lib/libworker)
target_link_libraries(my-worker PRIVATE worker)
```

사본은 그 워커의 것이라 워커 폴더 하나만 들고 나가도 빌드된다. 원본이 좋아졌을 때 가져올지는 워커마다 정한다(새 파일을 덮어쓰고 그 워커를 시험).
