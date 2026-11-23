# 시스템 모니터 (`sysmon`, 기본 탑재)

Jetson의 CPU·GPU·메모리·온도·전력을 `tegrastats`로 읽어 지표로 보고한다. 판정하지 않는다. 기록은 workspace-logger, 표시는 REST와 화면("보드 상태")이 한다.

**워커 폴더가 아니라 플랫폼 부품이다.** 모든 보드에 하나씩 있고 Manager가 `system-monitor`으로 띄운다(기본 탑재 워커). 설정은 이 폴더의 `config.json`·`presets/`(워커 폴더와 같은 방식). 워커 틀(`workers/_template`)과 같은 구성의 C++ 워커다: libworker 호스트 + 모듈 `sysmon`(보드 상주 블록은 C++, 상주 메모리 2~5 MB). 감시·재실행·기록·그래프는 워커와 같은 경로를 쓴다. 화면의 워커 목록에는 나오지 않고 "보드 상태"로만 보인다.

| 항목 | 값 |
|---|---|
| type / id | `sysmon` / `system-monitor`. 선언: `monitor/worker-types.json`의 `builtin`(자동 등록), `requires`(tegrastats) |
| 실행 파일 | `monitor/bin/sysmon` (C++ 확정 실행 파일, Jetson aarch64, `dev/release-worker.sh monitor`). 개발 빌드가 있으면 그것이 먼저(`exec_candidates`: `build/sysmon` → `build/jetson/install/bin/sysmon` → `bin/sysmon`) |
| 필요한 것 | `tegrastats` (JetPack 기본 포함). 없으면(PC 등) 상태 `disabled`(띄우지 않음) |

```text
monitor/
├─ CMakeLists.txt      이 폴더만으로 빌드(native·jetson-build)
├─ src/main.cpp        Host + 모듈 sysmon, 장치 오류 → exit 2
├─ lib/
│  ├─ libworker/       실행 뼈대 사본(원본 workers/_template/lib/libworker, 같은지 dev/test_workers.py가 확인)
│  └─ sysmon/          ① core/(tegrastats 줄 → 지표, /proc·/sys 지표. libworker 모름) ② include·src(모듈) test/(코어 유닛 시험)
├─ test/test_worker.cpp  통합 시험(가짜 tegrastats·수집기 파일 → READY·지표·exit code·SIGTERM 정리)
├─ bin/                확정 실행 파일 sysmon + BUILD(소스 커밋·sha256)
├─ worker-types.json   type 선언(builtin: 자동 등록, requires: 필요한 도구)
├─ config.json, presets/
├─ design.md
└─ README.md
```

## 빌드·확정

```bash
cmake -S monitor -B ~/mon-build && cmake --build ~/mon-build -j && ctest --test-dir ~/mon-build --output-on-failure   # PC 빌드·시험
cd monitor && jetson-build --test        # Jetson 크로스 빌드 + QEMU 시험(가짜 tegrastats 시험은 건너뜀)
dev/release-worker.sh monitor            # 커밋한 소스로 → monitor/bin/sysmon + BUILD(스테이지됨) → git commit
```

개발 중 보드 반영: `jetson-build` 결과를 보드 `~/dev-workspace/monitor/build/sysmon`에 복사하고 `system-monitor`만 다시 시작(`exec_candidates`가 먼저 고름). 지우고 다시 시작하면 `bin/`으로 돌아간다. `dev/check.sh`가 PC 빌드·시험과 TSan 빌드를 돌린다.

## 설치

따로 할 것이 없다. `install_workspace.sh`가 tegrastats가 있는지만 확인한다. 설정을 바꾸면 이 폴더의 `presets/default.json`에 저장된다(깃에 올라감).

## 설정 (`config`)

| 키 | 기본값 | 내용 |
|---|---|---|
| `sysmon.interval_ms` | `1000` | 측정 주기. 100 이상. 예전 형식(맨 위 `interval_ms`)도 받는다(로그에 `old config format` 경고 한 줄) |
| `watchdog_sec` | `3` | 자체 워치독(호스트 키, 1 이상). 호스트 루프가 `watchdog_sec` 동안 돌지 않으면 커널이 워커를 끝낸다. 직접 실행에서 `tegrastats`가 `watchdog_sec` + 한 주기 넘게 출력이 없으면 exit 2. Manager가 다시 실행한다 |

그 밖의 호스트 키(`stop_timeout_sec`, `thread_stall_sec`, `command_socket` 등)는 libworker 공통(워크스페이스 design.md 6.6.3).

## 명령 (REST)

정비 모드에서 보드의 REST(`:8080`)로 보낸다. 아래 예는 보드에서 실행하는 경우다.

| 동작 | 명령 |
|---|---|
| 상태·지표 | `curl -s localhost:8080/workers/system-monitor \| python3 -m json.tool` |
| 시작·정지 | `curl -X POST localhost:8080/workers/system-monitor/start`, `.../stop` |
| 주기 변경 | `curl -X PUT localhost:8080/workers/system-monitor/config -H 'Content-Type: application/json' -d '{"sysmon": {"interval_ms": 500}}'` |
| 워커 상태 | 명령 `get_status` → `sysmon`: 출처(`source`, `collector`), `interval_ms`, `lines`, `power_mode`, 최신 지표(`metrics`) |

## 지표

`tegrastats`에 나오는 항목만 보고한다. 보드와 JetPack 버전에 따라 다를 수 있다. 이름·단위·값 글자는 옛 Python 판과 같다(모듈 접두어 없이). heartbeat는 1초마다 최신 표본(표본은 tegrastats 줄마다). libworker 공통 지표 `host_loop_ms`, `host_errors`, `host_budget_overruns`가 함께 나간다.

| 지표 | 내용 |
|---|---|
| `ram_used_mb`, `ram_total_mb`, `swap_used_mb` | 메모리 |
| `cpu_avg_pct`, `cpu_max_pct`, `cpu_online` | 온라인 코어 평균·최대 사용률, 온라인 코어 수 |
| `cpu_mhz_max` | 온라인 코어 최고 클럭 |
| `gpu_pct` | GPU(GR3D) 사용률 |
| `emc_pct` | 메모리 대역폭 사용률 (root 수집기일 때만) |
| `nvenc_mhz`, `nvdec_mhz`, `nvjpg_mhz`, `vic_mhz`, `nvdla0_mhz`, `nvdla1_mhz`, `ofa_mhz` | 전용 HW 클럭. `0`이면 꺼짐 (root 수집기일 때만) |
| `temp_<부위>_c`, `temp_max_c` | 온도 |
| `power_<레일>_mw` | 레일별 전력(레일 이름은 소문자). `power_vdd_in_mw`가 보드 전체 입력 |
| `power_mode` | nvpmodel 모드 번호 |
| `cpu<N>_pct`, `cpu<N>_mhz` | 코어별 사용률·클럭(꺼진 코어는 빠짐) |
| `gpu_mhz` | GPU 클럭 |
| `swap_total_mb` | SWAP 전체 |
| `disk_used_gb`, `disk_total_gb` | `/` 사용량 |
| `net_rx_kbps`, `net_tx_kbps` | 기본 경로 인터페이스 수신·송신(영상 송출 대역 확인) |
| `fan_pct`, `fan_rpm` | 팬 PWM·회전수(장치가 있을 때) |
| `uptime_s` | 가동 시간 |
| `collector_root` | `1`: 시작 셸의 root 수집기를 읽는 중, `0`: 직접 실행(EMC·전용 HW 항목 없음) |

EMC·전용 HW 항목은 `tegrastats`를 root로 돌려야 나온다. 그래서 `workspace-start.sh`가 root 수집기를 띄워 최신 한 줄을 `~/dev-workspace_data/run/tegrastats.line`에 두고, 이 워커는 일반 계정으로 그 파일을 읽는다. 수집기가 멈추면 exit 2로 끝나고, Manager가 다시 띄우면 직접 실행으로 바뀐다.

종료 코드: `0` 정상 정지, `1` 설정 오류, `2` `tegrastats` 없음·비정상 종료·출력 멈춤·알아볼 수 없는 출력·수집기 멈춤(재시작하면 낫는 경우라 스스로 종료), `3` 명령 소켓 등 자체 자원 실패, `4` 내부 실패, 시그널 `SIGALRM` 자체 워치독 만료.

## 문제 해결

| 증상 | 원인과 조치 |
|---|---|
| `collector_root`가 `0` | root 수집기가 없거나 멈춤. 워크스페이스를 다시 시작(`workspace-start.sh`(실행 중이면 멈추고 다시 시작)) |
| 계속 exit 2 | `which tegrastats`로 확인. JetPack(L4T) 기본 도구라 없으면 보드 이미지 문제 |

로그: `tail -f ~/dev-workspace_data/state/logs/system-monitor.log`

## 기록과 지난 기록 보기

모든 지표는 workspace-logger가 1초마다 `~/dev-workspace_data/records/*.jsonl`에 남긴다(파일당 64 MB, 전체 2 GB를 넘으면 오래된 파일부터 지움 — 하루 약 45 MB라 약 45일분). 기간별로 뽑을 때:

```bash
python3 ~/dev-workspace/logger/workspace_history.py --id system-monitor --minutes 60 --points 360
```
```bash
python3 ~/dev-workspace/logger/workspace_history.py --id system-monitor --minutes 1440 --points 0 --keys cpu_avg_pct,temp_max_c --csv > sysmon.csv
```

워크스페이스 UI 보드 상태 패널의 "추이"에서 기간(실시간 3분, 1시간, 6시간, 24시간)을 고르고 CSV로 내려받을 수 있다. 프로세스 목록(CPU 순)도 같은 패널에 있다.
