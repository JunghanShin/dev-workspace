# 시스템 모니터 설계서 (`sysmon`, 기본 탑재)

| 항목 | 내용 |
|---|---|
| 대상 | Jetson 보드 상태(CPU·GPU·메모리·온도·전력·전용 HW) |
| 실행 파일 | `bin/sysmon` (C++, libworker 호스트 + 모듈 `sysmon`). 소스 `src/`, `lib/sysmon/` |
| 사용법 | `README.md` |
| 상태 | 운용 중(예전 Python `sysmon.py`를 지표 이름·값·동작 그대로 C++로 옮김) |

워크스페이스 플랫폼 부품(`monitor/`)이다. 실행·감시는 워커와 같은 계약(heartbeat, exit code, 자체 워치독)을 따르고, Manager가 기본 탑재 워커로 자동 등록한다. 보드에 늘 상주하는 블록이라 C++(워크스페이스 design.md 1장 원칙 10: Python 12~25 MB → C++ 2~5 MB). 구성은 워커 틀과 같다(층 ① 코어 / ② 모듈·호스트). 워커 폴더가 아니므로 ③ `workspace/`는 없고, type 선언·설정은 폴더 맨 위(`worker-types.json`, `config.json`, `presets/`), 확정 실행 파일은 `bin/`(manager·gateway와 같은 방식).

## 1. 실행 구조

```text
 ┌ sysmon 프로세스(호스트 루프 하나, 스레드 없음) ────────────────────────────────────┐
 │                                                                                      │
 │  출처 ① root 수집기          <데이터>/run/tegrastats.line (시작 셸의 root tegrastats)  │
 │        on_timer: interval/4마다 mtime → 바뀌면 파일 통째로 읽음 → 한 줄               │
 │  출처 ⓪ 직접 실행            자식 tegrastats --interval N ─stdout 파이프─▶ poll        │
 │        on_poll: 읽기(non-blocking) → 줄 나누기 → 한 줄                                │
 │                                                                                      │
 │  한 줄 ─▶ parse_tegrastats(코어) ─▶ + power_mode + SystemExtras.sample(코어: /proc·/sys)│
 │        ─▶ + collector_root ─▶ 최신 표본(latest)                                       │
 │                                                                                      │
 │  호스트(libworker): 1 s마다 WATCHDOG progress=<줄 수> + 최신 표본(접두어 없이)         │
 │                     + host_* · 명령 소켓(get_status) · 자체 워치독 · 종료 절차         │
 └──────────────────────────────────────────────────────────────────────────────────────┘
        │ heartbeat(지표)                     자식 tegrastats는 같은 프로세스 그룹
        ▼
 Manager → workspace-logger(기록), 게이트웨이 → 화면 "보드 상태"
```

| 부분 | 위치 | 하는 일 |
|---|---|---|
| ① 코어 | `lib/sysmon/core/` | `parse_tegrastats`(줄 → 지표), `SystemExtras`(디스크·네트워크·팬·가동 시간), `read_power_mode`, `MetricList`(순서·값 글자). libworker 모름 |
| ② 모듈 | `lib/sysmon/include·src` | 출처 고르기, 첫 줄 기다리기, 수집기 감시·자식 관리, 장치 오류 판정, 상태·지표 |
| ② 호스트 | `src/main.cpp`, `lib/libworker`(사본) | 계약 전부. 장치 오류로 끝나면 exit code를 2로 |

가벼운 감시라 단일 루프로 둔다(병렬 파이프라인 대상 아님). 막힐 수 있는 곳(자식 출력 읽기)은 non-blocking 파이프를 호스트 루프의 `poll`로 기다려 루프가 막히지 않는다.

## 2. 동작

- JetPack 기본 `tegrastats`(주기 `interval_ms`, 기본 1000)의 한 줄을 지표로 바꿔 heartbeat로 보고. 판정하지 않음
- EMC·전용 HW(NVENC·VIC·DLA 등) 항목은 tegrastats를 **root로** 실행해야 나온다. 그래서 시작 셸이 root 수집기(`install/bin/workspace-tegrastats-collect`)를 띄워 최신 한 줄을 `<데이터>/run/tegrastats.line`에 둔다(매번 통째로 교체, 크기 고정). sysmon은 일반 계정으로 이 파일을 읽는다
- 시작할 때 수집기 파일이 interval의 3배 안에 바뀌었으면 수집기, 아니면 직접 tegrastats를 실행한다(EMC·전용 HW 항목 없음). 수집기 파일이 interval의 3배 넘게 바뀌지 않으면 exit 2 → Manager 재시작 시 직접 실행으로 바뀐다. 어느 쪽인지는 지표 `collector_root`(1/0)
- READY는 알아볼 수 있는 첫 줄을 읽은 뒤(모듈 `start()`가 첫 줄까지 기다리고, 그 뒤 호스트가 READY). 5 s + interval 넘게 알아볼 수 없는 줄만 오거나 5 s + interval + `watchdog_sec` 동안 줄이 없으면 exit 2
- 실행 중 직접 실행의 tegrastats가 `watchdog_sec` + interval 넘게 출력이 없거나 끝나면 exit 2
- 표본은 tegrastats 줄마다(네트워크 속도는 직전 표본 대비), heartbeat는 1 s마다 최신 표본
- 플랫폼 부품(`monitor/`). Manager가 `system-monitor`으로 띄우는 기본 탑재 워커(설정은 이 폴더의 `config.json`·`presets/`). tegrastats가 없으면 `disabled`
- exit: 1 설정 오류, 2 `tegrastats` 없음·비정상 종료·출력 멈춤·수집기 멈춤, 3 명령 소켓 실패, 4 내부 실패. SIGTERM 시 `tegrastats` 정리(SIGTERM → 2 s → SIGKILL) 후 0

## 3. 인터페이스 명세 (ICD)

### 3.1 설정

| 키 | 기본 | 내용 |
|---|---|---|
| `sysmon.interval_ms` | 1000 | 측정 주기(100 이상). tegrastats `--interval`, 수집기 확인 간격(1/4), 멈춤 판정(×3) |
| (맨 위) `interval_ms` | — | 옛 Python 판 형식. 모듈 칸으로 옮겨 읽고 경고 한 줄(`host.legacy_keys`) |
| `watchdog_sec` | 3 | 호스트 키(1 이상). 자체 워치독, 직접 실행의 출력 멈춤 판정(+ interval) |

### 3.2 지표 (heartbeat, 접두어 없음)

| 지표 | 의미 |
|---|---|
| `ram_used_mb`, `ram_total_mb`, `swap_used_mb`, `swap_total_mb` | 메모리 |
| `cpu_avg_pct`, `cpu_max_pct`, `cpu_online`, `cpu_mhz_max` | 온라인 코어 평균(소수 한 자리)·최대 사용률, 코어 수, 최고 클럭 |
| `cpu<N>_pct`, `cpu<N>_mhz` | 코어별(꺼진 코어 `off`는 빠짐) |
| `emc_pct` | 메모리 대역폭 사용률 |
| `gpu_pct`, `gpu_mhz` | GPU(GR3D) 사용률·클럭 |
| `nvenc_mhz`, `nvdec_mhz`, `nvjpg_mhz`, `vic_mhz`, `nvdla0_mhz`, `nvdla1_mhz`, `ofa_mhz` | 전용 HW 클럭. 0 = 꺼짐(미사용) |
| `temp_<부위>_c`, `temp_max_c` | 온도(센서 없음 표시 -256C는 제외) |
| `power_<레일>_mw` | 레일별 현재 전력(`VDD_*`, `VIN_*`). `power_vdd_in_mw` = 보드 입력 전체 |
| `power_mode` | nvpmodel 모드 번호(`/var/lib/nvpmodel/status`를 시작할 때 읽을 수 있으면) |
| `disk_used_gb`, `disk_total_gb` | `/`(소수 한 자리) |
| `net_rx_kbps`, `net_tx_kbps` | 기본 경로 인터페이스(`/proc/net/route`, metric 최소)의 수신·송신(소수 한 자리) |
| `fan_pct`, `fan_rpm` | hwmon `pwmfan`/`pwm1`(0~255 → %), `pwm_tach`/`rpm` |
| `uptime_s` | `/proc/uptime` |
| `collector_root` | 1 = root 수집기 사용(전 항목), 0 = 직접 실행(EMC·전용 HW 없음) |
| `host_loop_ms`, `host_errors`, `host_budget_overruns` | libworker 공통 |

- 표기는 JetPack 버전에 따라 다르다. 받는 꼴은 `lib/sysmon/core/include/sysmon/tegrastats.hpp` 머리말(JP4·JP5·JP6 형식). 없는 항목은 빠진다
- 정규식·값 글자는 옛 Python 판과 같다(정수는 그대로, 온도는 Python `repr(float)`처럼 `45.0`, `46.937`, 반올림 값은 `%.1f`). 옛 판과 3000줄 넘는 변형 입력으로 같은 출력임을 확인함
- 옛 판처럼 받지 않는 표기: `VDDQ_*` 같은 `VDD_`/`VIN_`로 시작하지 않는 레일, JP4 `POM_*` 전력(mW 단위 없음), `NVJPG1`·`PVA0` 등 목록 밖 엔진

### 3.3 명령·상태

명령은 libworker 공통(`get_status`)만. `get_status`의 `sysmon`: `source`(`root collector`/`own tegrastats`), `collector_root`, `collector`(파일 경로), `interval_ms`, `lines`(받은 표본 수), `power_mode`, `metrics`(최신 표본).

## 4. 감시

| 항목 | 값 |
|---|---|
| `start_timeout_sec` / `watchdog_sec` / `stop_timeout_sec` (Manager, `worker-types.json`) | 10 s / 5 s / 3 s |
| 자체 워치독 `watchdog_sec` | 3 s(호스트 루프). 첫 줄을 기다리는 동안은 5 s + interval + `watchdog_sec` + 1 |
| 출력 멈춤(직접 실행) | `watchdog_sec` + interval 동안 줄 없음 → exit 2 |
| 수집기 멈춤 | interval × 3 동안 파일이 안 바뀜 → exit 2 |

## 5. 시험

| 시험 | 내용 |
|---|---|
| `lib/sysmon/test/test_core.cpp` | 코어 유닛: JP4·JP5·JP6 줄과 일부·잘못된 줄 → 지표 이름·글자·순서, 가짜 `/proc`·`/sys` 나무 → 디스크·네트워크(직전 대비, 인터페이스 바뀜)·팬·가동 시간·전원 모드 |
| `test/test_worker.cpp` | 통합: 가짜 tegrastats(셸)로 READY 시점·지표·`--interval`·SIGTERM 자식 정리, 설정 오류 exit 1, tegrastats 없음·알아볼 수 없는 출력·비정상 종료·출력 멈춤 exit 2, 수집기 파일(EMC·전용 HW, 멈춤 exit 2, 오래된 파일이면 직접 실행). Jetson QEMU 시험은 수집기·설정 시험만 |

`dev/check.sh`가 PC 빌드·시험과 TSan 빌드를, `dev/release-worker.sh monitor`가 `jetson-build --test`(크로스 빌드 + QEMU)를 돌린다.
