# 워크스페이스 설정 파일

보드의 설정은 전부 **데이터 폴더의 `config/`**(예: `~/dev-workspace_data/config/`)에 있다. 워크스페이스 코드(저장소)에는 현장 값이 없다.

- **워커 설정**(장비 주소, 송출, FC 메시지 주기 등)은 화면에서 바꾼다. 각 패널의 설정 버튼, 또는 시스템 모니터 워커 목록의 설정 버튼(design.md 6.10)
- **워크스페이스 자체 설정**(이 문서)은 SSH로 파일을 직접 고친다. 잘못 바꾸면 화면으로 되돌릴 수 없는 값(IP, 포트, FC 라우터)이라 화면에서는 바꾸지 않는다

## 파일이 생기는 곳

| 어디서 | 언제 |
|---|---|
| 프로필 `install/profiles/<이름>/` | 설치할 때 그 프로필을 고르면 들어 있는 파일을 넣는다. 처음 한 번만(보드에 이미 있으면 그대로, 되돌리려면 `--reset-config`). 만드는 법: [README "프로필"](../README.md#프로필) |
| 기본값 `install/config/` | 데이터 폴더에 없는 파일만 시작할 때 채운다. 기본값은 "아무것도 하지 않음"(고정 IP 끔, 원격 중계 끔) |

## 파일 한눈에

| 파일 | 정하는 것 | 형식 | 바꾼 뒤 |
|---|---|---|---|
| `network.conf` | 보드 고정 IP | `KEY=값` | `workspace-start.sh` (또는 재부팅) |
| `workspace.env` | REST 포트, 화면 제목·영상 주소, 기록 크기 | `KEY=값` | `workspace-start.sh` |
| `manager.json` | Manager 동작(재시작 간격, 워커 감시 기본값) | JSON(바꿀 값만) | `workspace-start.sh` |
| `mavlink-router.conf` | FC 시리얼 포트·속도, MAVLink 출구 | mavlink-router 문법 | `sudo systemctl restart workspace-mavlink-router` |
| `mediamtx.yml` | 로컬 미디어 서버, 원격 중계 | YAML | `sudo systemctl restart workspace-mediamtx` |

`workspace-start.sh`는 실행 중이면 멈추고 다시 시작한다(워커·화면이 잠깐 끊김).

**고치지 않는 파일**(워크스페이스가 만든다): `worker-types.json`, `worker-types.d/`(시작할 때마다 워커 폴더에서 다시 만듦), `mavlink-router.d/`, `backup-<시각>/`(설치가 남긴 이전 설정), `profile.json`(설치가 넣은 프로필 기록).

**화면에서 보기:** 설정 > **워크스페이스**가 지금 보드의 설정을 읽기 전용으로 보여 준다. 적용한 프로필(이름·시각), 위 파일마다 내용과 출처(프로필 그대로 / 기본값 / 보드에서 바뀜), `workspace.env` 키마다 보드 값·기본값·설명. 고친 뒤 다시 시작했으면 여기서 반영됐는지 확인한다. 비밀값(이름에 PASS·TOKEN·SECRET·API_KEY)은 가려서 보인다.

`workspace.env`의 설명은 기본 파일 `install/config/workspace.env`의 주석에서 온다. 새 키를 더할 때 그 파일에 키 바로 위 주석(설명)과 함께 넣는다. `## `로 시작하는 줄은 화면 표의 묶음 제목이다.

## 파일별

### `network.conf` — 고정 IP

```bash
ENABLED=yes                       # yes일 때만 적용. 기본 no(보드의 기존 네트워크 그대로)
IFACE=enP8p1s0                    # 인터페이스 이름(ip link 로 확인)
ADDRESSES="192.168.10.100/24"     # 고정 주소(CIDR). 여러 개면 공백으로
GATEWAY=192.168.10.1              # 다른 망으로 나가는 라우터. 비우면 설정 안 함
DNS=                              # 비우면 설정 안 함
```

- 시작할 때마다 root로 적용한다(`nmcli`, `install/bin/workspace-network-apply`)
- **주의:** SSH로 접속한 인터페이스의 주소를 바꾸면 접속이 끊긴다. 새 주소로 다시 접속하고, 안 되면 콘솔(USB-C `192.168.55.1`)에서 `ENABLED=no`로 되돌린다
- SSH 포트는 sshd 기본(22) 그대로다. 외부 포트 변환은 보드 밖(라우터)에서 한다

### `workspace.env` — 실행 설정

| 키 | 기본 | 뜻 |
|---|---|---|
| `WORKSPACE_REST_BIND` | `0.0.0.0` | REST·화면을 여는 주소. `127.0.0.1`이면 보드 안에서만(SSH 터널로만 접속) |
| `WORKSPACE_REST_PORT` | `8080` | REST·화면 포트. 바꾸면 화면 주소도 바뀐다 |
| `WORKSPACE_REST_COMMAND_TIMEOUT` | `3` | 워커 명령 응답을 기다리는 초. 장치 응답을 기다리는 명령보다 길게 |
| `WORKSPACE_UI_MEDIA_URL` | `http://{host}:8889/{path}` | 화면의 영상 주소 틀. `{host}` = 브라우저가 접속한 보드 주소, `{path}` = 송출 경로. 원격 미디어 서버를 보려면 그 주소로 |
| `WORKSPACE_UI_TITLE` | `Workspace` | 화면 왼쪽 위·브라우저 탭 제목 |
| `WORKSPACE_LOG_LEVEL` | `INFO` | Manager·REST·logger 로그 수준(`DEBUG`, `INFO`, `WARNING`) |
| `WORKSPACE_LOGGER_INTERVAL_S` | `1` | 기록 간격(초) |
| `WORKSPACE_LOGGER_MAX_FILE_MB` | `64` | 기록 파일 하나의 크기 |
| `WORKSPACE_LOGGER_MAX_TOTAL_MB` | `2048` | 기록 전체 상한. 넘으면 오래된 파일부터 지움 |
| `WORKSPACE_REST_TOKEN` | 없음 | REST 접근 토큰. 있으면 `/health`·화면 파일 말고 모두 토큰 필요(`Authorization: Bearer`, 쿠키 `workspace_token`, `?token=`). 제품 설치가 장비별로 만든다 |
| `WORKSPACE_UI` | `1` | `0`이면 보드가 화면을 내주지 않음(별도 제어 UI가 REST만 씀) |

- `KEY=값` 한 줄씩. 셸로 실행하지 않고 읽기만 한다. 경로(소켓, 데이터 폴더)는 넣지 않는다

### `manager.json` — Manager 동작

바꿀 값만 적는다(나머지는 기본값). 예:

```json
{
  "restart": {"interval_s": 5},
  "worker_defaults": {"watchdog_sec": 5}
}
```

| 키 | 기본 | 뜻 |
|---|---|---|
| `restart.interval_s` | `2.0` | 실패한 워커를 다시 시작하는 간격(횟수 제한 없음) |
| `worker_defaults.start_timeout_sec` / `watchdog_sec` / `stop_timeout_sec` | `10` / `3` / `5` | 워커 type이 정하지 않았을 때의 시작 대기·워치독·종료 대기 |
| `log.max_bytes`, `log.backups` | 10 MB, 3 | 워커 로그(`state/logs/<워커>.log`) 회전 |
| `api.*`, `tick_s` | | 내부 값. 바꿀 일 없음 |

### `mavlink-router.conf` — FC 연결

```ini
[UartEndpoint flight_controller]
Device = /dev/ttyTHS1        # FC TELEM2. 처음 설치할 때 FC가 응답한 포트로 채운다
Baud = 921600                # FC의 SER_TEL2_BAUD와 같게

[UdpEndpoint server]         # 원격 서버(GCS 등). 쓰려면 추가
Mode = Normal
Address = 192.168.1.10
Port = 14540

[UdpEndpoint local_workspace]  # 워크스페이스 FC 워커가 받는 곳. 지우지 않는다
Mode = Normal
Address = 127.0.0.1
Port = 14551
```

- `#`으로 시작하는 줄만 주석(줄 끝 주석 불가). 자세한 것: [fc-mavlink-router.md](fc-mavlink-router.md)
- mavlink-router가 워크스페이스 밖에서 따로 설치된 보드는 이 파일이 아니라 그 설치의 설정(예: `/etc/mavlink-router/main.conf`)을 쓴다. 설치할 때 알려 준다

### `mediamtx.yml` — 미디어 서버

- 워커 송출을 받아(`rtsp://127.0.0.1:8554/<경로>`) 로컬 브라우저에 보여 준다(WebRTC `:8889`, HLS `:8888`)
- **원격 미디어 서버로 중계:** 파일 끝 `paths:`에서 `all_others: {}`를 지우고 그 아래 주석 세 줄을 풀어 `<원격 미디어 서버>`를 넣는다. 들어온 경로를 같은 이름으로 그대로 보낸다
- 여기 없는 항목은 MediaMTX 기본값. 자세한 것: [local-ui-media.md](local-ui-media.md)

### 워커 설정은 여기 없다

워커 설정은 데이터 폴더가 아니라 **워커 폴더**에 있다: `workers/<워커>/workspace/config.json`(사용 중 프리셋, 자동 시작)과 `presets/<이름>.json`(설정 묶음). 화면의 워커 설정 창에서 적용(임시)·저장·프리셋 고르기를 한다. 남기거나 나눌 설정은 직접 커밋한다(워크스페이스는 깃에 쓰지 않음). 키는 각 워커 README "설정"(design.md 6.3.1). 예전 `workers.json`이 있으면 처음 시작할 때 워커 폴더로 옮기고 `workers.json.moved`로 이름을 바꾼다.

## 바꾸는 순서

1. SSH로 보드에 접속해 데이터 폴더 `config/`의 파일을 고친다(먼저 복사해 두면 되돌리기 쉽다)
   ```bash
   cp ~/dev-workspace_data/config/network.conf ~/network.conf.bak
   ```
2. 위 표의 "바꾼 뒤" 명령으로 적용한다
3. 확인한다: `journalctl -u workspace -n 50`, 화면(`http://<보드>:8080`)
4. 계속 쓸 값이면 프로필에도 남긴다: 데이터 폴더의 파일을 `install/profiles/<이름>/`으로 가져와 커밋. 다른 보드는 그 프로필로 설치하면 같은 값이 된다. 따라 하는 순서는 [README "프로필"](../README.md#프로필)

**되돌리기:** 프로필 값으로 되돌리려면 `sudo bash <워크스페이스>/install/install_workspace.sh --profile <이름> --reset-config`(지금 파일은 `config/backup-<시각>/`에 남는다). 파일을 지우고 다시 시작하면 기본값(`install/config/`)으로 다시 만든다.
