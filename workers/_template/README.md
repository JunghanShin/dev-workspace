# 워커 틀 (`_template`)

새 워커는 이 틀로 만든다. 틀 자체가 **바로 빌드되고 워크스페이스에 붙는 예제 워커**(`example-worker`)다. 실제 기능은 없고(값 생성·평균), 워커가 갖출 것을 모두 보여 준다. 워커 폴더 하나에 코드·라이브러리·설계·설치를 모두 두어 **폴더 하나만 들고 나가도 빌드·실행된다.**

| 모듈 | 보여 주는 것 |
|---|---|
| `source` | 장치 자리. 스레드 둘(`source.gen` → 큐 → `source.pub`), 데이터 버스 발행(`raw f32x<size>`), 스트림 주제, 명령, 바로 적용, 자식 프로세스(`helper_cmd`) |
| `processor` | 같은 워커 안 모듈 연결: `source`의 포트(`SourcePort`)를 받아 이동 평균 |
| `peer` | 다른 워커와 통신 세 통로: 데이터 버스(InOrder, 빠짐 검사)·스트림 구독·명령 넘기기. 설정 `peer`(worker_id)가 비면 꺼짐 |

워커 안은 자유롭게 설계한다: 스레드 여럿, 자식 프로세스 여럿, 모듈 여럿. 틀은 그 각각의 본보기를 하나씩 둔다. 필요 없는 모듈은 뗀다.

규칙 전체: 워크스페이스 `design.md` 3장 "단계", 6.6(계약), 6.6.1(병렬 구조), 6.6.3(호스트 + 모듈), 6.6.4(층 나누기), 6.6.7(워커 간 통신).

## 단계와 시험

| 단계 | 여기서 | 경계 규칙 | 시험 |
|---|---|---|---|
| 워커(CSCI) | 이 폴더 | 다른 워커와는 명령·스트림·데이터 버스로만(상대는 설정 칸 `worker_id`). 주고받는 것은 `design.md` 3장(ICD) | `test/test_worker.cpp`: 실행 파일을 띄워 명령·상태, 워커 둘 통신, 상대가 다시 시작해도 다시 붙음 |
| 모듈(CSC) | `lib/<모듈>/` | 다른 모듈과는 포트로만 | `lib/<모듈>/test/`: 호스트 없이 `configure`·`call_module`·가짜 포트 |
| 유닛(CSU) | `lib/<모듈>/core/` | libworker를 모름 | `lib/<모듈>/test/`: 코어만, 가짜 시계 |

보드에서는 서브시스템 시험 `dev/system-check.sh`. 워크스페이스 `dev/test_workers.py`가 층 규칙(코어가 libworker를 쓰면 실패)과 미준수 목록(유닛 시험이 없는 모듈 등, 알림만)을 낸다.

## 세 층

워커는 처음부터 세 층으로 나눠 만든다. 쓰는 곳에 따라 아래 층만 가져간다.

| 층 | 무엇 | 어디 | 의존 |
|---|---|---|---|
| ① 기능 코어 | 그 워커의 일(장치 프로토콜, 궤적 계산, 영상 처리) | `lib/<모듈>/core/` | 표준 C++(필요하면 JSON 헤더). **libworker를 모른다** |
| ② 워커 실행 | 코어를 모듈로 감싸기(설정 칸·명령·상태·지표·스레드), 실행 루프, 명령·스트림 소켓, 워치독 | `lib/<모듈>/include·src`, `lib/libworker`, `src/main.cpp` | libworker 실행 뼈대. **워크스페이스를 모른다** |
| ③ 워크스페이스 연동 | type 등록·설정 화면 스키마·준비·설치·확정 실행 파일·패널, Manager heartbeat·데이터 폴더 경로 | `workspace/`, libworker 빌드 옵션 `WORKER_WORKSPACE` | 워크스페이스 |

| 쓰는 곳 | 가져가는 것 | 할 일 |
|---|---|---|
| 워크스페이스 안 | 전부 | 그대로 |
| 다른 보드에서 단독 실행 | ① + ② | `workspace/` 삭제, `-DWORKER_WORKSPACE=OFF`로 빌드. 실행 방법·설정 파일·명령은 같다 |
| 기능만 다른 프로그램에 | ① | `lib/<모듈>/core/` 복사 |

## 폴더 구성

```text
workers/<이름>/
├─ CMakeLists.txt            이 폴더만으로 빌드
├─ README.md, design.md      사용법, 설계 (dev/new-worker.sh가 틀에서 만들어 줌)
├─ io.json                   입출력 정의 목록: 내보내는 정의(provides), 쓰는 정의(uses). 아래 "입출력 정의 쓰기"
├─ io/<정의>.json            내보내는 정의 원본(받는 명령·발행 주제·데이터 버스, 칸은 JSON Schema)
├─ io/uses/<정의>.json       쓰는 정의 사본(다른 워커의 것)
├─ src/main.cpp              호스트 + 모듈 조립(얇음)                                   ②
├─ test/test_worker.cpp      워커 통합 시험(실행 파일, 워커 둘)
├─ lib/
│  ├─ libworker/             실행 뼈대 사본(호스트·모듈·스레드·큐·소켓·워치독)         ②
│  │  └─ src/workspace/      Manager heartbeat·데이터 폴더 경로(WORKER_WORKSPACE=ON일 때만) ③
│  ├─ <모듈>/
│  │  ├─ core/               기능 코어(include/<모듈>/, src/)                          ①
│  │  ├─ include/<모듈>/, src/  모듈: 코어를 worker::Module로 감싸기                   ②
│  │  ├─ schema.json         설정 화면 칸 원본(workspace/worker-types.json으로 옮김)   ③
│  │  ├─ README.md           하는 일, 설정 키, 명령, 상태, 지표, 스레드, 포트
│  │  └─ test/               유닛 시험(코어만) + 모듈 시험(호스트 없이)
│  ├─ libframebus/           데이터 버스 사본(영상 프레임·원시 레코드)
│  ├─ libvideo/              영상 송출 사본(송출하는 워커만)
│  └─ lib<장치>/             장치 라이브러리(예: 장치 프로토콜 구현, 외부 헤더)
├─ tools/                    CLI 도구(있으면)
├─ build/                    빌드 결과(git 제외)
└─ workspace/                워크스페이스 연동 전부. 밖에서 쓰면 폴더째 삭제              ③
   ├─ worker-types.json      type, 실행 파일 자리(@WORKER_EXEC@), config_schema, live_keys
   ├─ config.json            사용 중 프리셋, 자동 시작 {"active": "default", "autostart": false}
   ├─ presets/<이름>.json    설정 묶음 {"label": "기본", "config": {...}}. 화면에서 저장, 깃에 올라감
   ├─ prepare.sh             시작할 때마다: 실행 파일을 골라 type 등록
   ├─ install.sh             1회 설치: 패키지·udev·SDK, 확정 실행 파일 확인
   ├─ bin/                   확정 Jetson 실행 파일 + BUILD (dev/release-worker.sh)
   └─ panel/                 이 워커의 화면 패널(panel.json, panel.js). 워크스페이스 화면이 여기서 읽는다
python/workspace_worker.py   Python 워커 도우미 원본(new-worker.sh는 복사하지 않음. Python 워커가 직접 복사)
```

- `lib/libworker`·`libframebus`·`libvideo`는 그 워커의 사본이다. 이 틀의 것이 원본이다. 원본을 고치면 그 라이브러리를 쓰는 워커에 모두 덮어쓰고 시험한다. 사본이 원본과 같은지는 `dev/test_workers.py`가 확인한다(워커 쪽에서 고칠 일이 생기면 원본에 먼저 반영)
- 워커는 모두 이 구성으로 만든다(`dev/test_workers.py`가 폴더 구성을 확인). 예전 구성(워커 폴더 맨 위의 `worker-types.json`, `prepare.sh`, `install/`, `bin/`, 패널은 `ui/web/panels/`)도 워크스페이스가 계속 읽는다
- 송출하는 장치 워커는 libvideo의 송출 모듈을 붙인다: `host.add<video::StreamsModule>([&cam] { return cam.stream_defaults(); })`(설정 `{"stream": {"streams": [...]}}`, 명령 `stream.start|stop`, `lib/libvideo/README.md`)

## 새 워커 만들기

워크스페이스 폴더에서(PC WSL 또는 보드):

1. 틀 복사. `<폴더>`는 워커 폴더·id·실행 파일(`<폴더>-worker`)·패널 id. 모듈(source·processor·peer)은 이름 그대로 온다
   ```bash
   dev/new-worker.sh thermal            # 송출하는 장치 워커면 --video (libvideo 사본도)
   ```
2. 빌드·시험(그대로 돈다: 예제 모듈 셋, 워커 둘 통신)
   ```bash
   cd workers/thermal && jetson-build --native --test
   ```
3. 설계 먼저: `design.md` 2장(모듈·유닛·스레드·프로세스), 3장(ICD: 명령·상태·지표·스트림·데이터 버스·다른 워커와의 연결·설정)
4. 워커의 일 쓰기
   - 모듈 이름 정하기: 예제 모듈을 고쳐 쓰거나(폴더·namespace·CMake target 이름을 바꿈) 새 모듈을 만들고("모듈 만들기·붙이기") 필요 없는 예제 모듈은 뗀다("모듈 떼기")
   - `lib/<모듈>/core/`: 그 워커의 일(순수 C++, 유닛). 시험은 `lib/<모듈>/test/`
   - `lib/<모듈>/src/module.cpp`: 설정 키, 명령, 상태, 지표, 기다리는 일은 `<모듈>.<역할>` 스레드
   - `lib/<모듈>/schema.json`(원본) → `workspace/worker-types.json`의 `config_schema` 묶음(`"section": "<모듈>"`)
   - `test/test_worker.cpp`: 워커의 명령·상태·연결 시험(예제 시험을 고쳐 씀)
   - `workspace/panel/panel.js`: 화면
   - `README.md`, `design.md`: 설정·명령·지표·스레드 표를 채운다
   - `workspace/presets/default.json`: 기본 설정 값(화면에서 저장하면 여기에 기록)
5. 워크스페이스에서 보기: PC는 `dev/workspace-dev.sh`, 보드는 `workspace-start.sh`로 다시 시작하면 시스템 모니터·설정 화면·패널에 나온다. 워커 폴더가 있으면 붙는다(등록 단계 없음, 처음엔 자동 시작 안 함). 시스템 설치(`workspace/install.sh`)가 있으면 보드에서 `install_workspace.sh`를 먼저
6. 보드에서 돌려 보기(반복): `dev/deploy-worker.sh thermal`(Jetson 빌드 → 보드 `build/` → 그 워커만 다시 시작). 끝나면 `--revert`로 확정 실행 파일로 돌림
7. 검사: `dev/check.sh`(Python 시험 + 템플릿·모든 워커 PC 빌드·시험), 보드에서 `dev/system-check.sh`
8. 확정: 소스를 커밋한 뒤 `dev/release-worker.sh thermal`(Jetson 빌드·시험 포함) → `workspace/bin/` 커밋

## 워커 = 호스트 + 모듈

워커는 **호스트**(`lib/libworker`의 `worker::Host`, 워커 계약을 전부 맡음)에 **모듈**을 붙여 만든다. `main.cpp`는 모듈을 만들어 잇기만 한다(이 틀의 `src/main.cpp`):

```cpp
int main(int argc, char** argv) {
    worker::Host host("example-worker");
    auto& src = host.add<source::Module>();
    host.add<processor::Module>(src.port());   // 모듈끼리는 포트(작은 인터페이스)로만
    host.add<peer::Module>();                  // 다른 워커와 통신(상대는 설정 peer.peer)
    return host.run(argc, argv);
}
```

| 이름 | 규칙 | 예 (모듈 `link`) |
|---|---|---|
| 설정 칸 | `config["<모듈>"]` | `{"link": {"listen": "127.0.0.1:9000"}}` |
| 명령 | `<모듈>.<명령>` | `link.set_mode`, 상태는 `get_status` → `data.link` |
| 지표 | `<모듈>_<이름>`(호스트가 붙임) | `link_up`, `link_loss_pct` |
| 스레드 | `<모듈>.<역할>` | `link.rx`, `link.tx`, `link.pub` |

## 모듈 만들기·붙이기

이미 있는 워커에 모듈을 더할 때. 나중에 기억이 안 나도 이 순서대로 하면 된다. 아래 예는 모듈 `motion`, 워커 `siyi`. 본보기: 틀의 `lib/source`(스레드·발행·자식 프로세스), `lib/processor`(포트 받기, 가짜 포트 시험), `lib/peer`(다른 워커와 통신).

### 1. 모듈 폴더

```text
lib/motion/
├─ CMakeLists.txt        motion_core(core/만, libworker 없음) + motion(모듈, worker 링크) 두 라이브러리
├─ README.md             하는 일, 설정 키, 명령, 지표, 스레드, 포트
├─ schema.json           설정 화면 칸 원본({"section": "motion", "fields": [...]} 또는 묶음이 여럿이면 {"section", "groups": [{"title", "fields"}]}, 워크스페이스 design.md 6.10 형식)
├─ core/include/motion/  기능 코어 헤더(예: trajectory.hpp)     ①
├─ core/src/             기능 코어 구현(헤더만이면 없어도 됨: CMake INTERFACE 라이브러리, 예 lib/peer)  ①
├─ include/motion/module.hpp   class Module : public worker::Module   ②
├─ include/motion/port.hpp     다른 모듈이 쓰는 포트(있으면)
├─ src/module.cpp
└─ test/test_motion.cpp  코어 단독 + 모듈(호스트 없이)
```

### 2. 코어와 모듈 쓰기

- **코어(①):** 시각·입력을 인자로 받고 결과를 돌려주는 순수 코드. 소켓·스레드·지표 문자열·`worker::` 아무것도 쓰지 않는다. 잘못된 값은 코어 자신의 예외로
- **모듈(②):** `worker::Module`을 상속해 필요한 것만 채운다

```cpp
class Module : public worker::Module {
public:
    explicit Module(GimbalPort& gimbal) : gimbal_(gimbal) {}       // 다른 모듈의 포트는 생성자로 받는다
    std::string name() const override { return "motion"; }        // 설정 칸·명령·지표·스레드 앞부분
    void configure(const worker::Json& c) override {               // 자기 칸만 온다. 잘못되면 worker::ConfigError
        worker::check_keys(c, {"accel_dps2", "control_period_ms"});
        worker::read(c, "accel_dps2", cfg_.accel_dps2);
        core_.set_config(cfg_);                                    // 코어 예외는 ConfigError로 바꿔 던진다
    }
    void start(worker::Host& host) override {}                     // 소켓·스레드(host.watch()로 WorkerThread)
    void on_timer(worker::TimePoint now) override { core_.step(now); }
    worker::TimePoint next_wakeup(worker::TimePoint now) const override { return now + period_; }
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override {
        if (cmd != "move_to") return false;                        // 모르는 명령은 false
        reply(worker::ok_reply({{"accepted", true}}));             // 응답은 정확히 한 번(나중에라도)
        return true;
    }
    worker::Json status() const override { return {{"moving", core_.moving()}}; }   // get_status의 "motion" 칸
    void metrics(worker::Metrics& m) const override { m.set("moving", core_.moving()); }  // → motion_moving
};
```

- 기다리거나 무거운 일(장치 응답 대기, 인코딩 등)은 `start()`에서 `worker::WorkerThread("motion.ctl", host.watch(), ...)`로 따로 돌리고, 호스트 쪽과는 `worker::BoundedQueue`로만 주고받는다(본보기 `lib/source`: `source.gen` → 큐 → `source.pub`)
- 따로 떠야 하는 프로그램(ffmpeg, 다른 언어 도구 등)은 자식 프로세스로: `worker::spawn`·`worker::kill_wait`, 죽으면 모듈이 다시 띄운다(본보기 `source` `helper_cmd`). 워커가 끝나면 Manager가 프로세스 그룹째 정리한다
- 포트: 다른 모듈에 내주는 것은 `include/<모듈>/port.hpp`의 작은 인터페이스(본보기 `source::SourcePort`). 받는 모듈은 그 헤더만 include하고(CMake에서 그 모듈의 `include/`만), 시험에서는 가짜 포트를 넣는다(본보기 `processor/test`)
- 되돌릴 수 없는 실패(스레드 예외)는 `failed()`에서 참을 돌려준다. 호스트가 끝내고 Manager가 다시 띄운다

### 3. 워커에 붙이기

1. `CMakeLists.txt`에 `add_subdirectory(lib/motion)`, 워커 실행 파일에 `target_link_libraries(<워커> PRIVATE motion)`
2. `src/main.cpp`에서 `host.add<motion::Module>(...)`. 다른 모듈의 포트가 필요하면 그 모듈을 먼저 `add`하고 참조를 넘긴다
3. `workspace/worker-types.json`의 `config_schema`에 묶음을 더한다. 칸은 모듈 `schema.json`의 `fields`(또는 `groups`의 묶음마다)를 그대로 복사하고 `"section": "motion"`을 붙인다. 같은지는 `manager/test_schema.py`가 확인한다
   ```json
   {"title": "동작 제어", "section": "motion", "fields": [ ...lib/motion/schema.json의 fields... ]}
   ```
4. 패널(`workspace/panel/panel.js`)에서 명령을 `motion.<명령>`, 상태를 `data.motion`으로 쓴다
5. 프리셋(`workspace/presets/*.json`)의 `config`에 `"motion": {...}` 칸을 넣는다(없으면 모듈 기본값). 화면 설정 창에서 저장해도 된다

### 4. 시험

1. 유닛·모듈(PC): `test/test_motion.cpp`에서 코어는 직접(유닛), 모듈은 `configure()`·`worker::call_module(m, "move_to", {...})`·`on_timer(가짜 시각)`·가짜 포트
2. 워커 전체(PC): `test/test_worker.cpp`에 명령·상태 확인을 더한다. `jetson-build --native --test`, 워크스페이스에서 `dev/check.sh`(스키마·프리셋·모듈 schema.json, 폴더 구성·사본 라이브러리·패널 id 검사 + 모든 워커 빌드·시험)
3. 워크스페이스 없이: `cmake -S . -B build/standalone -DWORKER_WORKSPACE=OFF && cmake --build build/standalone && (cd build/standalone && ctest)`
4. Jetson: `dev/release-worker.sh <워커>`(`jetson-build --test`를 직접 돌림) → `workspace/bin/` 커밋
5. 보드: 시스템 모니터 워커 줄을 펼쳐 스레드 표에 `motion.*` 스레드, 지표 그래프에 `motion_*`가 보이는지

### 5. 모듈 떼기

`main.cpp`의 `host.add` 한 줄, `CMakeLists.txt`의 두 줄, `config_schema`의 묶음, `live_keys`의 그 모듈 키, 프리셋의 칸, 패널·`test/test_worker.cpp`에서 그 모듈을 쓰는 곳, 폴더 `lib/<모듈>`을 지운다. 다른 모듈이 그 포트를 쓰고 있으면 같은 포트를 구현한 모듈로 바꿔 끼운다.

예: 다른 워커와 통신하지 않는 워커는 `peer`를 뗀다. 데이터 버스도 안 쓰면 `lib/libframebus`도 지운다(`source`가 쓰므로 `source`를 고친 뒤).

## 안전망과 제공 도구 (design.md 6.6.9)

실행 구조(스레드·큐·우선순위·잠금)는 워커 개발자가 정한다. 워크스페이스는 받쳐 주고 도구를 준다.

- **기본 안전망(항상 켜짐, 할 일 없음):** 호스트가 모듈의 모든 진입점 예외를 잡는다(명령 하나는 오류 응답, `on_timer`·`on_poll`이 연달아 3번이면 exit 4, 꼭 끝내야 하면 `throw worker::Fatal`). 루프 시간 지표 `host_loop_ms`. 스레드가 멈추면 워치독보다 먼저 `safe_state()`를 부르고 exit 4. 장치를 안전하게 둘 일이 있으면 `Module::safe_state()`만 채운다(예: `lib/source`)
- **제공 도구(골라 씀):** 입력·출력 단계 `host.input<T>` / `host.output<T>`(예: `lib/source`의 `source.gen`), 스레드 우선순위·CPU(`ThreadOptions`, 설정 `stages`), 우선순위 상속 잠금 `worker::Mutex`, ThreadSanitizer(`dev/check.sh`가 모든 워커 시험을 TSan으로도 돌림, 빼려면 시험에 라벨 `no_tsan`), 장애 주입(시험 빌드 `WORKER_FAULT_INJECTION=1`의 `__fault` 명령, `test/test_worker.cpp` 4단계)
- 설정 예(맨 위 호스트 키): `"stages": {"source.gen": {"queue": 64, "priority": "realtime", "rt_priority": 50, "cpu": [2]}}, "loop_budget_ms": 50`
- 시스템 모니터에서 워커를 고르면(보기 드롭다운) 스레드 우선순위, 단계·큐, 안전망이 보인다

## 워커 간 통신 (design.md 6.6.7)

| 통로 | 받는 쪽 도구 | 본보기(`lib/peer`) |
|---|---|---|
| 요청·응답 | `worker::CommandClient`(`worker/peer.hpp`): 별도 스레드가 상대 명령 소켓으로 보내고 응답은 큐로 | 명령 `peer.set_rate` → 상대 `source.set_rate`, 응답이 오면 그때 `reply`(대기 중 요청은 번호로 보관) |
| 신호·상태 | `worker::Subscriber`(`worker/peer.hpp`): 상대 스트림을 구독, 끊기면 다시 붙음 | 상대 주제를 `status`의 `peer_topics`로 |
| 큰 데이터 | `framebus::Consumer(path, Mode::InOrder 또는 Latest)` | 스레드 `peer.data`가 순서대로 전부 받고 번호 빠짐을 셈(`SeqTracker`) |

- 상대는 설정 칸으로: 스키마 칸에 `"format": "worker_id"`. 화면이 워커 id 목록을 주고, Manager가 상태 `peers`로 내놓는다. 코드에 워커 id를 박지 않는다
- 상대가 없거나 다시 시작해도 살아서 기다리며 다시 붙는다. 연결은 `<모듈>_link`(모듈 이름이 link면 `link_up`) 지표로
- 두 도구의 `wake_fd()`를 `add_pollfds`에 넣고 `on_poll`에서 `next()`로 꺼낸다(워커 루프는 기다리지 않음)
- 시험: `test/test_worker.cpp`처럼 실행 파일 둘(a, b의 `peer.peer = "a"`)을 띄우고, 소켓 위치는 `WORKSPACE_DATA`·`XDG_RUNTIME_DIR`을 임시 폴더로

### 입출력 정의 쓰기·가져오기 (design.md 6.6.7.1~6.6.7.2)

워커끼리 주고받는 것(받는 명령, 발행 주제, 데이터 버스)은 **정의 파일이 계약**이다. 쓰는 쪽은 상대 설계서를 읽고 칸 이름을 손으로 옮기지 않고 정의 사본을 가져다 쓴다.

```text
io.json                  {"provides": ["example"], "uses": {"upstream": {"from": "example", "version": "1.0", ...}}}
io/example.json          내보내는 정의(원본): commands·topics·data, 칸은 JSON Schema(+ unit)
io/uses/example.json     쓰는 정의(사본). 여기서는 peer가 같은 종류의 다른 워커를 받아서 자기 정의의 사본
```

1. **내보내기:** 다른 워커가 쓸 명령·주제·데이터 버스를 `io/<정의>.json`에 적는다. 정의 이름은 워커 이름이 아니라 역할(같은 역할의 다른 워커가 같은 정의를 내보내면 서로 바꿔 쓸 수 있음). `io.json`의 `provides`에 이름을 넣는다
2. **가져오기:** 다른 워커의 정의 파일을 `io/uses/<정의>.json`에 복사하고, `io.json`의 `uses.<입력 이름>`에 `from`·`version`·쓰는 `topics`·`commands`·`data`·`required`를 적는다. 코드는 사본에 있는 칸만 쓴다
3. **바꿀 때:** 칸·명령·주제를 더하면 작은 버전(1.0 → 1.1), 빼거나 이름·형식을 바꾸면 큰 버전(2.0). 정의를 바꾸면 쓰는 워커의 사본도 새로 가져온다
4. **설계서:** 3장은 칸 목록을 다시 적지 않고 정의 파일을 가리킨다(뜻·시점·이유·상대 없을 때 동작만)
5. **점검:** `dev/check.sh`(`dev/test_workers.py`)가 형식, `io.json`이 가리키는 파일, `uses`가 쓰는 항목, 사본이 원본과 같은지·호환되는지 본다

상대 주소를 설정 `inputs`로 받고 Manager가 연결 선언으로 채우는 부분(design.md 6.6.7.3~6.6.7.5)은 구현 예정이다. 그때까지 상대는 지금처럼 설정 칸(`worker_id`)으로 정한다.

## 설정·프리셋 (워커 폴더, design.md 6.3.1)

워크스페이스는 워커 목록도 워커 설정도 들고 있지 않다. 설정은 워커 폴더 `workspace/config.json`(사용 중 프리셋, 자동 시작)과 `workspace/presets/<이름>.json`이다. 화면 설정 창에서 **적용**(저장 안 함, 이번 부팅 동안), **저장**(사용 중 프리셋에), 프리셋 고르기, **새 이름으로 저장**, 이름 바꾸기·삭제를 한다. 저장하면 워커 폴더 파일만 바뀌고, 남길 설정은 직접 커밋한다(워크스페이스는 깃에 쓰지 않음). 프리셋 파일 이름은 영문 소문자·숫자·`-`·`_`, 화면 이름은 `label`.

## 바로 적용 설정 (재시작 없이)

설정이 바뀌면 기본은 워커 재시작이다. 재시작 없이 반영할 수 있는 칸은:

1. `workspace/worker-types.json`의 `live_keys`에 `"<모듈>.<키>"`를 적는다(예: `["source.rate_hz", "source.amplitude", "processor.window"]`)
2. 모듈에서 `bool reconfigure(const worker::Json& section)`을 구현한다. 반영했으면 `true`, 반영할 수 없는 키가 바뀌었으면 아무것도 바꾸지 말고 `false`(예제 `source`: `size`·데이터 버스·`helper_cmd`가 바뀌면 `false`). 잘못된 값은 `worker::ConfigError`

Manager가 SIGHUP을 보내면 호스트가 설정 파일을 다시 읽어 바뀐 모듈의 `reconfigure`를 부르고, 모두 반영하면 heartbeat `config_version`으로 알린다. 못 하면 Manager가 5초 뒤 재시작한다.

## 텔레메트리 발행 (화면 실시간 값)

계속 바뀌는 값은 `get_status` 폴링 대신 발행한다(design.md 6.6.2, 9.7). 예제 모듈 `source`가 본보기다: `start()`에서 `worker::Publisher`(스레드 `<모듈>.pub`)를 `host.stream_socket()`에 열고, `on_timer`에서 구독자가 있을 때만 `{"seq", "kf", "t", "topics": {"<주제>": {...}}, "events": [], "events_next": 0}` 한 줄을 `publish`. 새 구독자(`keyframe_requested()`)와 5초마다 `kf: true`. 패널은 `EventSource("/api/board/workers/<id>/stream?hz=10")`(예제 패널 `startStream`).

## 가짜 장치 모드 (장비 없이 개발, design.md 6.6.8)

장비가 없거나 다른 사람이 쓰는 중이어도 워커를 워크스페이스에서 돌린다. 화면 설정 창에서 프리셋 `sim`을 고르면 된다.

1. `tools/<이름>-sim.cpp`: 장치 흉내. 장치가 쓰는 통로를 127.0.0.1에 열고 실제 장치 형식으로 답하거나 데이터를 보낸다. 시험의 가짜 장치를 헤더 하나(`lib/<모듈>/test/fake_<장치>.hpp`)로 두고 시험과 함께 쓰면 둘이 같아진다
2. `CMakeLists.txt`: `add_executable(<이름>-sim ...)`, `install(TARGETS <이름>-sim RUNTIME DESTINATION bin)`(실행 파일 옆에 배포)
3. `workspace/presets/sim.json`: `"sim": {"cmd": "@EXEC_DIR@/<이름>-sim <인자>"}` + 장치 주소를 127.0.0.1로. SDK 라이브러리를 바꿔 끼우는 장치는 프로그램 없이 `"sim": {}` + 라이브러리 경로(예: `sdk_lib` = `@EXEC_DIR@/libfake_<sdk>.so`)
4. `test/test_worker.cpp`: 가짜 장치 모드로 띄워 붙는지(예: 틀의 3단계)

호스트가 가짜 장치를 자식으로 띄우고(죽으면 2초 뒤 다시), 워커 상태·화면에 "가상"으로 표시한다. 모듈은 보통 설정만 다르고, 필요하면 `host()->sim()`.

## Python 워커

계약(design.md 9장)은 Python으로도 지킬 수 있다. `python/workspace_worker.py`를 워커 폴더에 복사해 `load_config`, `Watchdog`, `Heartbeat`(`ready()`, `watchdog(progress, metrics)`), `StopFlag`를 쓴다(예: `monitor/sysmon.py`). 사본이 원본과 같은지 `dev/test_workers.py`가 확인한다.

워커 간 통신(design.md 6.6.7)도 C++과 같은 통로·같은 이름으로 쓴다. 형식이 언어와 무관해서 C++ 워커와 그대로 주고받는다:

| 하는 일 | Python(`workspace_worker`) | C++ |
|---|---|---|
| 명령 받기 | `CommandServer(path, handler)` + 루프에서 `serve(timeout)` | 호스트 `CommandServer` |
| 상태 발행 | `Publisher(path)` + `pump()`·`publish(dict)`·`keyframe_requested()` | `Publisher` |
| 다른 워커에 명령 | `CommandClient(peer_id)`: `send()` → 번호, `next()` → (번호, cmd, 응답) | `CommandClient` |
| 다른 워커 구독 | `Subscriber(peer_id)`: `next()` → `{down, kf, topics, events}` | `Subscriber` |
| 데이터 버스 받기 | `FramebusConsumer(path, mode="latest"/"inorder")`: `connect()`, `next(ms)`, `release(f)` | `framebus::Consumer` |

소켓 기본 위치는 `default_command_socket(id)`, `default_stream_socket(id)`, `default_socket(id, "frames")`(C++과 같은 규칙). 데이터 버스 생산자는 아직 C++(libframebus)만. 시험: `python/test_comm.py`(C++ 템플릿 워커와 양방향, `dev/check.sh`가 돌림).

## 틀 시험

```bash
cd workers/_template && jetson-build --native --test
```

```bash
cd workers/_template/lib/libworker && jetson-build --native --test
```
