# 시스템 모니터 설계서 (`sysmon`, 기본 탑재)

| 항목 | 내용 |
|---|---|
| 대상 | Jetson 보드 상태(CPU·GPU·메모리·온도·전력·전용 HW) |
| 실행 파일 | `sysmon.py` (Python 3) |
| 사용법 | `README.md` |
| 상태 | 운용 중 |

워크스페이스 플랫폼 부품(`monitor/`)이다. 실행·감시는 워커와 같은 계약(heartbeat, exit code, 자체 워치독)을 따르고, Manager가 기본 탑재 워커로 자동 등록한다. 가벼운 감시라 단일 루프로 둔다(병렬 파이프라인 대상 아님).

## 1. 동작

- JetPack 기본 `tegrastats`(주기 `interval_ms`, 기본 1000)의 한 줄을 지표로 바꿔 heartbeat로 보고. 판정하지 않음
- EMC·전용 HW(NVENC·VIC·DLA 등) 항목은 tegrastats를 **root로** 실행해야 나온다. 그래서 시작 셸이 root 수집기(`install/bin/workspace-tegrastats-collect`)를 띄워 최신 한 줄을 `<데이터>/run/tegrastats.line`에 둔다(매번 통째로 교체, 크기 고정). sysmon은 일반 계정으로 이 파일을 읽는다
- 시작할 때 수집기 파일이 최신이 아니면 sysmon이 직접 tegrastats를 실행한다(EMC·전용 HW 항목 없음). 수집기 파일이 interval의 3배 넘게 바뀌지 않으면 exit 2 → Manager 재시작 시 직접 실행으로 바뀐다. 어느 쪽인지는 지표 `collector_root`(1/0)
- 플랫폼 부품(`monitor/`). Manager가 `system-monitor`으로 띄우는 기본 탑재 워커(설정은 이 폴더의 `config.json`·`presets/`). tegrastats가 없으면 `disabled`
- exit: 1 설정 오류, 2 `tegrastats` 없음·비정상 종료. SIGTERM 시 `tegrastats` 정리 후 0

## 2. 지표

| 지표 | 의미 |
|---|---|
| `ram_used_mb`, `ram_total_mb`, `swap_used_mb` | 메모리 |
| `cpu_avg_pct`, `cpu_max_pct`, `cpu_online`, `cpu_mhz_max` | 온라인 코어 평균·최대 사용률, 코어 수, 최고 클럭 |
| `emc_pct` | 메모리 대역폭 사용률 |
| `gpu_pct` | GPU(GR3D) 사용률 |
| `nvenc_mhz`, `nvdec_mhz`, `nvjpg_mhz`, `vic_mhz`, `nvdla0_mhz`, `nvdla1_mhz`, `ofa_mhz` | 전용 HW 클럭. 0 = 꺼짐(미사용) |
| `temp_<부위>_c`, `temp_max_c` | 온도(센서 없음 표시 -256C는 제외) |
| `power_<레일>_mw` | 레일별 현재 전력. `power_vdd_in_mw` = 보드 입력 전체 |
| `power_mode` | nvpmodel 모드 번호(`/var/lib/nvpmodel/status`를 읽을 수 있을 때) |
| `collector_root` | 1 = root 수집기 사용(전 항목), 0 = 직접 실행(EMC·전용 HW 없음) |

- 표기는 JetPack 버전에 따라 다르다. 없는 항목은 빠진다(JP5·JP6 형식 확인)

## 3. 감시

| 항목 | 값 |
|---|---|
| `start_timeout_sec` / `watchdog_sec` / `stop_timeout_sec` (이식처) | 10 s / 5 s / 3 s |
| 자체 워치독 `watchdog_sec` | 3 s + 측정 주기 |
