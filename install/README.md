# 설치·실행

시스템 폴더(/etc, /opt, /var, /run)는 쓰지 않는다(design.md 11장). 전체 사용법은 [README](../README.md).

- 코드: 워크스페이스 폴더(받은 폴더, 예: `~/dev-workspace`). 여기서 바로 실행한다
- 데이터: 옆 폴더 **`<워크스페이스 폴더>_data`**(예: `~/dev-workspace_data`). 설정·상태·기록·SDK. 워크스페이스를 통째로 바꿔도 유지되고, 지워지면 시작할 때 다시 만든다. 워크스페이스 폴더 이름을 바꾸면 새 데이터 폴더로 시작한다. 설정 파일(`config/`)별 키·바꾸는 순서: [docs/config.md](../docs/config.md)
- 프로필: 프로젝트·현장의 워크스페이스 설정(망, 포트, 미디어 서버, 워커가 쓰는 설정)을 모은 폴더 `install/profiles/<이름>/`. `install_workspace.sh`가 실행할 때 이름을 묻고(Enter = `default`) 데이터 폴더에 넣는다
- 시스템에 남기는 것: 서비스 파일 2개(`/etc/systemd/system/workspace.service`, `workspace-mediamtx.service`), 의존 패키지(ffmpeg 포함), 장비별 설치(카메라 udev 규칙 등, 프로젝트 워커가 넣는 것)
- 워커별 설치·준비 파일은 각 워커 폴더(`workers/<워커>/`)에 있다. 이 폴더는 공통 부분만 갖는다
- **워커는 등록하지 않는다.** 워커 = 워커 폴더. 설치는 모든 워커 폴더의 시스템 설치(`workspace/install.sh`)만 하고, 워크스페이스가 시작할 때 워커 폴더를 보고 붙인다. 빼려면 폴더를 지우고 다시 시작
- **워커 설정은 워커 폴더의 `workspace/config.json`(사용 중 프리셋, 자동 시작)과 `workspace/presets/<이름>.json`(설정 묶음)이다. 화면에서 적용·저장하고 깃으로 공유한다(design.md 6.3.1)**
- **워크스페이스 설정은 데이터 폴더에 남는다.** 다시 설치해도 프로필이 덮어쓰지 않는다(되돌리려면 `--reset-config`)

## 폴더

| 경로 | 내용 |
|---|---|
| `../workspace-start.sh`, `../workspace-stop.sh` | 시작(실행 중이면 멈추고 다시 시작)·종료(워크스페이스 최상위). systemd도 시작 셸만 실행 |
| `install_workspace.sh` | 브링업: 프로필 선택·적용(실행 시 묻기, `--profile <이름>`), 공통 패키지, 모든 워커의 시스템 설치(`workers/<워커>/workspace/install.sh`), 프로필 되돌리기(`--reset-config`), 전원 모드, MediaMTX, 서비스 파일 |
| `install_mediamtx.sh` | 로컬 미디어 서버(MediaMTX) 설치: 워커 영상 로컬 시청(+ 켜면 원격 중계). 이미 설치되어 있으면 건너뜀. `--check`는 상태만 확인. [docs/local-ui-media.md](../docs/local-ui-media.md) |
| `systemd/workspace.service` | 서비스 파일 원본 |
| `systemd/workspace-mediamtx.service` | MediaMTX 서비스 파일 원본. 워크스페이스와 독립(자동 재시작) |
| `mediamtx/` | MediaMTX v1.21.3(arm64) 공식 압축 파일. 설치할 때 같은 폴더에 `mediamtx`를 풀고 서비스가 바로 실행. sha256 고정 |
| `config/` | 데이터 폴더 설정 기본값(일반 값). 데이터 폴더에 없는 파일만 복사. 현장 값은 넣지 않는다 |
| `profiles/<이름>/` | 프로필(프로젝트·현장 설정). 설치할 때 이름을 고른다. 예: `myproj` |
| `bin/workspace-network-apply` | `network.conf` 적용(시작할 때 root로) |
| `bin/workspace-tegrastats-collect` | root tegrastats 수집기. 최신 한 줄을 `<데이터>/run/tegrastats.line`에 둠(sysmon이 읽음) |

## 새 보드

1. 워크스페이스 받기. 마지막 인자가 로컬 폴더 이름이다. 저장소가 비공개라 보드에 배포 키(`github-dev-workspace`)를 먼저 등록한다([README](../README.md) "설치" 2번)
   ```bash
   git clone github-dev-workspace:JunghanShin/dev-workspace.git ~/dev-workspace
   ```
3. 설치(프로필 이름을 묻는다. Enter = `default`, 묻지 않으려면 `--profile <이름>`):
   ```bash
   sudo bash ~/dev-workspace/install/install_workspace.sh
   ```
4. 시작(또는 재부팅):
   ```bash
   ~/dev-workspace/workspace-start.sh
   ```
   처음 시작하면 데이터 폴더의 빠진 설정이 기본값으로 채워진다. 프로필 없이 설치했으면 `<데이터>/config/`(network.conf 등)를 고친 뒤 다시 시작(`workspace-start.sh`)

설치는 다시 실행해도 된다. 워크스페이스 경로를 바꿨으면 다시 실행한다.

### 프로필

프로필은 프로젝트·현장마다 다른 워크스페이스 설정 파일을 모은 폴더다(워커 설정은 워커 폴더의 프리셋). `install/profiles/<이름>/`에 두고(예: `myproj`), 저장소 밖 폴더도 `--profile <폴더>`로 쓸 수 있다. 들어 있는 파일만 데이터 폴더에 복사한다.

| 파일 | 예 |
|---|---|
| `network.conf` | 보드 고정 IP |
| `workspace.env` | 화면 제목(`WORKSPACE_UI_TITLE`), REST 포트 |
| `mediamtx.yml` | 원격 미디어 서버 중계 |
| `manager.json` | Manager 설정 |

- 처음 설치할 때만 넣는다. 데이터 폴더에 이미 다른 내용이 있으면(보드에서 바꾼 값) 그대로 두고 알린다. `--reset-config`면 `config/backup-<시각>/`에 남기고 덮어쓴다
- 넣은 프로필은 `config/profile.json`에 기록한다(이름, 폴더, 시각, 워크스페이스 커밋, 넣은 파일, 유지한 파일). 화면 설정 > 워크스페이스에서 보인다. 이 기록 전에 설치한 보드는 화면이 보드 파일과 가장 많이 같은 프로필을 "추정"으로 보여 준다
- 미디어 서버 설정이 바뀌면 설치 끝에 그 서비스를 다시 시작한다(워커가 쓰는 서비스는 그 워커의 install.sh가 맡는다)
- 새 프로필 만들기·프로필로 설치·프로필 고치기(따라 하는 순서): [README "설정: 프로필과 데이터 폴더"](../README.md#설정-프로필과-데이터-폴더)
- PC 개발 실행도 같은 프로필을 쓴다: `dev/workspace-dev.sh --profile <이름>`

### 보드를 새로 플래시한 뒤

JetPack 6.2.x 이미지로 플래시했다면:

1. USB-C로 접속(`ssh <사용자>@192.168.55.1`)하고 PC의 SSH 키 등록(`ssh-copy-id`)
2. 위 "새 보드" 1~4단계. 고정 IP(프로필), 서비스, 워커 설치(SDK·udev 규칙), ffmpeg가 모두 여기서 설치된다
3. 필요하면 호스트 이름 변경: `sudo hostnamectl set-hostname <이름>`

## 시작·종료

```bash
bash ~/dev-workspace/workspace-start.sh
```
```bash
bash ~/dev-workspace/workspace-stop.sh
```

- root가 아니면 sudo를 묻는다
- `workspace-start.sh`는 이미 실행 중이면(서비스·`--foreground` 모두) 먼저 멈추고 다시 시작한다. `workspace-stop.sh`는 완전히 끌 때만
- **자동 재시작 없음.** Manager가 끝나면 전체가 내려가고 서비스는 멈춘 상태로 남는다. `workspace-start.sh`로 다시 시작
- 코드를 고쳤으면 다시 시작(`workspace-start.sh`). 설치는 다시 할 필요 없다
- 서비스 없이 터미널에서 실행(디버깅): `~/dev-workspace/workspace-start.sh --foreground`
- 상태·로그: `systemctl status workspace`, `journalctl -u workspace -n 100`

## 데이터 폴더 (`<워크스페이스>_data/`)

| 파일 | 내용 | 적용 |
|---|---|---|
| `config/network.conf` | 고정 IP(`ENABLED=yes`일 때). SSH는 sshd 기본 22, 외부 포트 변환은 보드 밖 | 다시 시작(`workspace-start.sh`) 또는 재부팅 |
| `config/workspace.env` | REST 바인딩·포트, 화면 영상 주소(`WORKSPACE_UI_MEDIA_URL`)·제목(`WORKSPACE_UI_TITLE`), 로그 레벨, 로거 주기·용량. 경로 키는 무시 | 다시 시작(`workspace-start.sh`) |
| `config/manager.json` | Manager 설정(design.md 14장 값 덮어쓰기) | 다시 시작(`workspace-start.sh`) |
| `config/mediamtx.yml` | 로컬 미디어 서버: 수신·시청 포트, 원격 중계 주소(`runOnAvailable`, 현장 값) | `sudo systemctl restart workspace-mediamtx` |
| `state/logs/<id>.log` | 워커 로그 | — |
| `run/temp_config.json` | 저장하지 않고 적용한 워커 설정(이번 부팅 동안, 재부팅하면 버림) | — |
| `records/*.jsonl` | record-manager 기록 | — |
| `sdk/<워커>/` | 워커가 쓰는 장치 SDK. 그 워커의 install.sh가 설치할 때 풂 | — |

- 파일을 지우면 다음 시작 때 기본값으로 다시 만든다. 폴더 전체를 지워도 된다(워커 설정은 워커 폴더에 있어 그대로). SDK는 `sudo bash <워크스페이스>/workers/<워커>/workspace/install.sh`로 다시 푼다
- 같은 구성을 다른 보드에 쓰려면 `install/profiles/<이름>/`으로 만들어 설치 때 고른다

워커 설정(워커 폴더 `workspace/config.json`, `workspace/presets/`) 주의:
- `config.json` 예: `{"active": "default", "autostart": false}`. 프리셋 예(`presets/default.json`): `{"label": "기본", "config": {"camera": {"fps": 30}}}`
- `autostart: true`인 워커만 부팅 시 자동으로 뜬다. `false`(위성 송출 등)는 `POST /workers/<id>/start` 명령이 있어야 돈다. `start`/`stop`은 그 부팅 동안만 유효
- 화면에서 저장하면 파일이 바뀐다(깃의 "고친 파일"). 남길 설정은 직접 커밋한다(워크스페이스는 깃에 쓰지 않음)
- 파일을 직접 고쳤으면 다시 시작(`workspace-start.sh`)
- 예전 `config/workers.json`이 있으면 처음 시작할 때 그 값을 워커 폴더로 옮기고 `workers.json.moved`로 이름을 바꾼다

`network.conf` 주의:
- IP가 바뀌면 지금 SSH 세션은 끊기므로 새 주소로 다시 접속
- 접속이 안 되면 콘솔에서 `ENABLED=no`로 고치고 재부팅

## 워커 폴더 추가 규칙

워커 폴더 `workers/<장비>/` 하나에 코드·실행 준비·설치를 모두 둔다. 워크스페이스 연동 파일은 그 안의 `workspace/`에 모은다(워커 틀 `workers/_template`, design.md 6.6.4). 새 워커는 `dev/new-worker.sh <폴더>`.

```text
workers/<장비>/
  <코드: src/, lib/<모듈>/core·모듈, lib/libworker …>
  workspace/
    worker-types.json   # type 정의(실행 파일 자리 @WORKER_EXEC@, config_schema, live_keys)
    config.json         # 사용 중 프리셋, 자동 시작
    presets/<이름>.json # 설정 묶음(프리셋)
    prepare.sh          # 시작할 때마다(root)
    install.sh          # 브링업 1회(root)
    bin/                # 확정 Jetson 실행 파일 + BUILD
    panel/              # 화면 패널(panel.json의 id)
```

| 파일 | 언제 | 내용 |
|---|---|---|
| `workspace/prepare.sh` | 시작할 때마다(root, `workspace-start.sh`) | 실행 파일을 골라(`build/` 먼저, 없으면 `workspace/bin/`) type 조각을 `<데이터>/config/worker-types.d/<폴더>.json`에. SDK 경로, 장치 설정. 여러 번 실행해도 같은 결과 |
| `workspace/worker-types.json` | — | 워커 type 조각 원본 |
| `workspace/config.json`, `workspace/presets/` | 시작할 때 Manager가 읽음, 화면에서 저장 | 워커 설정(design.md 6.3.1). 깃에 올라감 |
| `workspace/install.sh` | 브링업 1회(root, `install_workspace.sh`) | 시스템에 꼭 있어야 하는 것: udev 규칙, 패키지, SDK 풀기, 확정 실행 파일 확인(BUILD sha256). 환경 변수 `WORKSPACE_ROOT`, `WORKSPACE_DATA`를 받는다 |
| `workspace/panel/` | 화면을 열 때 | REST가 `/api/panels`에 넣고 `/panels/<id>/`로 준다 |
| `README.md` | — | 이 워커의 설치·설정(`config`)·명령(REST)·지표·문제 해결. 워커 폴더만 보고 쓸 수 있게 한다 |

- 워커 폴더를 넣고(시스템 설치가 있으면 `install_workspace.sh`) 다시 시작하면 붙는다. 등록 단계는 없다
- Manager·REST·기존 워커는 수정하지 않는다 (design.md 원칙 9)
- 워커가 SDK 경로가 필요하면 `prepare.sh`가 `$WORKSPACE_DATA/run/payload.env`에 `PYTHONPATH=…`, `LD_LIBRARY_PATH=…` 를 한 줄씩 추가
- 외부 SDK는 다운로드하지 않고 워커 폴더 안에 보관한다. 워커를 시험한 버전으로 고정하고 sha256으로 확인한다
- 예전 구성(폴더 맨 위 `prepare.sh`·`worker-types.json`, `install/install.sh`, `bin/`)도 읽는다. `_`로 시작하는 폴더(틀)는 설치·준비 대상이 아니다

## 현재 워커

워커마다 설치·설정·명령·지표는 그 폴더의 `README.md`에 있다.

| 워커 폴더 | 워커 type | 설명서 | 설치(`workspace/install.sh`) |
|---|---|---|---|
| `replay` | `replay_worker` | [workers/replay/README.md](../workers/replay/README.md) | 실행 파일 확인만 |
| `_template` | `example_worker` | [workers/_template/README.md](../workers/_template/README.md) | 틀(설치·준비 대상 아님). `dev/new-worker.sh <폴더>`로 새 워커를 만든다 |

## 기록·상태 보기

- 워커 상태·지표: `curl -s localhost:8080/workers/<id> | python3 -m json.tool` (`metrics`, `resources`)
- 지금 기록 따라 보기: `tail -f "$(ls -t <데이터>/records/*.jsonl | head -1)"`
- 명령 이력: `grep -h '"type":"command"' <데이터>/records/*.jsonl`
