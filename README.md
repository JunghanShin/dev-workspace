# dev-workspace

Jetson 보드에서 **워커**를 개발·실행·확인하는 개발 플랫폼입니다.

- **워커**: 페이로드 하나(장치 하나: 제어·캡처·영상 송출까지)를 맡는 독립 프로그램입니다. 폴더 하나로 들고 나갈 수 있습니다. 개발의 결과물입니다.
- **워크스페이스**: 워커를 띄우고 감시하는 시험대입니다. 상태·지표·로그 기록, 화면, 영상 시청, FC 연동을 제공합니다. 새 워커를 만들 때마다 이 도구들을 다시 만들 필요가 없습니다.

코드는 어느 프로젝트에서나 쓰는 플랫폼입니다. 프로젝트·현장마다 다른 값(장비 주소, 서버 주소, 쓰는 워커, 화면 제목)은 **프로필**(`install/profiles/<이름>/`)로 코드와 분리해 두고, 설치할 때 이름만 고릅니다([프로필](#프로필)).

## 목차

- [무엇을 하나](#무엇을-하나)
- [구성](#구성)
- [폴더 구조](#폴더-구조)
- [설치 (Jetson)](#설치-jetson)
- [매일 쓰는 법](#매일-쓰는-법)
- [프로필](#프로필)
- [데이터 폴더](#데이터-폴더)
- [화면과 영상](#화면과-영상)
- [워커 개발](#워커-개발)
- [PC 개발 환경](#pc-개발-환경)
- [포트와 주소](#포트와-주소)
- [문서 안내](#문서-안내)
- [문제 해결](#문제-해결)
- [진행 상황](#진행-상황)

## 무엇을 하나

| 기능 | 내용 |
|---|---|
| 워커 실행·감시 | 워커를 띄우고, 죽거나 멈추면(heartbeat 끊김) 다시 실행합니다. 부팅 시 자동 시작 여부를 워커마다 정합니다 |
| 원격 제어 | REST API(:8080)로 워커 시작·정지, 설정 변경, 명령(짐벌 이동 등)을 보냅니다 |
| 화면 | 브라우저로 보드 상태, 워커별 전용 패널, 로그, 명령 이력을 봅니다. 보드가 직접 제공하므로 보드에 붙은 PC는 브라우저만 열면 됩니다 |
| 영상 | 워커 영상을 보드 안의 미디어 서버(MediaMTX)로 받아 바로 봅니다. 필요하면 원격 미디어 서버로 중계합니다 |
| 기록 | 워커 상태·지표·명령 이력을 파일로 남기고, 지난 지표를 그래프와 CSV로 봅니다 |
| FC 연동 | FC(PX4)의 MAVLink를 mavlink-router로 받아 워크스페이스 프로그램이 쓸 수 있게 합니다 |
| 설치 | 저장소를 받아 설치 스크립트 한 번이면 같은 환경이 됩니다. 검증된 C++ 워커 실행 파일도 저장소에 들어 있습니다 |

## 구성

```text
 보드에 붙은 PC 브라우저          원격 PC·서버 UI          FC (PX4)
        │ http :8080                  │ http :8080           │ 시리얼
        ▼                             ▼                      ▼
┌─ Jetson ──────────────────────────────────────────────────────────────┐
│  REST (API + 화면) ─────────┐                    mavlink-router ──┐     │
│      │ 워커 관리            │ 워커 명령                (127.0.0.1:14551) │
│      ▼                     ▼                                         │
│  Manager ── 실행·감시 ──▶ [워커] 카메라 짐벌 FC system-monitor …        │
│      │                       │ 영상(RTSP)                             │
│  workspace-logger            ▼                                        │
│  (상태·명령 기록)          MediaMTX :8554 ──▶ 브라우저 WebRTC :8889     │
│                                    └──▶ 원격 미디어 서버(켰을 때)      │
└───────────────────────────────────────────────────────────────────────┘
```

| 구성 요소 | 역할 | 언어 |
|---|---|---|
| Manager | 워커 실행, 종료 감시, 재실행, 상태 보관 | Python |
| REST | HTTP API(:8080)와 화면 제공. 늘 켜짐 | Python |
| workspace-logger | 상태·지표·명령 이력 기록 | Python |
| 워커 | 장치 하나 또는 처리 단계 하나를 맡는 프로세스 | Python, C++ |
| MediaMTX | 보드 안 미디어 서버. 워커 영상 수신, 브라우저 시청, 원격 중계 | (공식 배포 실행 파일) |
| mavlink-router | FC MAVLink를 여러 출구(UDP)로 나눔 | (빌드한 실행 파일) |

역할 분담 원칙(design.md 5장):
- **워커**가 스스로 판단해 종료합니다. 멈춤은 워커 내부 워치독으로 감지합니다
- **Manager**는 관찰하고, 종료되거나 heartbeat가 끊긴 워커를 다시 실행하기만 합니다
- 외부 장치가 응답하지 않는 것은 종료 사유가 아닙니다. 워커는 살아서 상태를 보고합니다

## 폴더 구조

```text
dev-workspace/
├─ README.md                 이 문서
├─ design.md                 설계서(워커 계약, API, 장애 처리)
├─ workspace-start.sh        전체 시작
├─ workspace-stop.sh         전체 종료
├─ .jetson-dev-env           필요한 빌드 환경 버전
├─ manager/                  Manager (workspace_manager)
├─ rest/            REST (workspace_rest: API + 화면 제공)
├─ logger/                   workspace-logger, 지난 지표 조회(workspace_history.py)
├─ monitor/                  시스템 모니터 sysmon(기본 탑재: Manager가 자동 등록, 워커 폴더 아님)
├─ workers/                  워커 폴더. 각 폴더가 코드·라이브러리(lib/)·실행 준비·설치를 함께 갖는다(폴더 하나로 독립)
│  ├─ _template/             새 워커의 틀(표준 구성, lib/libworker = 공통 뼈대 원본)
│  ├─ replay/                데이터 버스 녹화·재생 (개발 도구, C++)
│  └─ <장치>/                장치 워커는 여기에 폴더째 넣는다(dev/new-worker.sh로 틀에서 만듦)
├─ ui/                       화면: web/(보드 REST가 제공하는 프론트엔드, 워커별 패널)
├─ install/                  설치 스크립트, 서비스 파일, 설정 기본값, mavlink-router·MediaMTX
├─ dev/                      PC 개발 실행(workspace-dev.sh), C++ 워커 확정(release-worker.sh)
├─ docs/                     설정 파일 안내, 기술 노트(FC 연동, 로컬 UI·영상)
└─ test/                     보드에서 돌리는 시험 도구(fc_test.py)
```

보드에 설치하면 옆에 **데이터 폴더** `<워크스페이스 폴더>_data/`가 생깁니다(예: `~/dev-workspace_data`). 설정·상태·로그가 여기에 쌓이며 저장소에는 들어가지 않습니다.

## 설치 (Jetson)

### 순서

1. **(FC를 쓰면) FC 설정 먼저.** QGC로 FC 파라미터를 맞추고 재부팅합니다(`MAV_1_CONFIG` = TELEM 2, `MAV_1_MODE` = Onboard, `SER_TEL2_BAUD` = 921600, `MAV_1_FLOW_CTRL` = Force off, `UXRCE_DDS_CFG` = Disabled). FC 안에 저장되는 값이라 Jetson 설치로는 바뀌지 않습니다. 자세한 것은 [docs/fc-mavlink-router.md](docs/fc-mavlink-router.md)
2. **보드에 GitHub 접근 열기(처음 한 번).** 저장소가 비공개라 보드에서 그냥 받으면 로그인을 요구합니다. 보드에 읽기 전용 배포 키를 만들고
   ```bash
   ssh-keygen -t ed25519 -N "" -C "<보드 이름> dev-workspace deploy" -f ~/.ssh/id_ed25519_dev_workspace
   ```
   ```bash
   printf '\nHost github-dev-workspace\n  HostName github.com\n  User git\n  IdentityFile ~/.ssh/id_ed25519_dev_workspace\n  IdentitiesOnly yes\n' >> ~/.ssh/config
   ```
   공개키(`cat ~/.ssh/id_ed25519_dev_workspace.pub`)를 GitHub 저장소 Settings → Deploy keys에 등록합니다. 화면에서 바꾼 워커 설정을 보드에서 깃에 올리려면 **Allow write access**를 켭니다(키 하나에 한 저장소)
3. **워크스페이스 받기.** 마지막 인자가 보드에 생길 폴더 이름입니다. 원하는 이름으로 정해도 되고(예: 프로젝트 이름 `~/<프로젝트>-workspace`), 데이터 폴더는 그 이름 뒤에 `_data`가 붙습니다. 아래 예는 `~/dev-workspace`
   ```bash
   git clone github-dev-workspace:JunghanShin/dev-workspace.git ~/dev-workspace
   ```
4. **설치(sudo).** 실행하면 프로필(`install/profiles/`의 이름)을 묻습니다. Enter = `default`, 묻지 않으려면 `--profile <이름>`. 워커는 묻지 않습니다: 워커 폴더가 있으면 모두 시스템 설치(udev·SDK 등)를 하고, 워크스페이스가 시작할 때 워커 폴더를 보고 붙입니다(등록 단계 없음). FC를 쓰면 FC 전원을 켠 채로 설치합니다(FC 포트를 자동으로 찾음)
   ```bash
   sudo bash ~/dev-workspace/install/install_workspace.sh
   ```
5. **시작.** 이후에는 부팅할 때 자동으로 시작합니다
   ```bash
   ~/dev-workspace/workspace-start.sh
   ```
6. **확인.** 같은 망의 PC 브라우저로 `http://<보드 주소>:8080`을 엽니다(USB-C 연결이면 `http://192.168.55.1:8080`)

### 설치가 하는 일

| 항목 | 내용 |
|---|---|
| 프로필 적용 | 고른 프로필의 설정 파일을 데이터 폴더에 넣음. **처음 한 번만**: 보드에 이미 있는 파일(화면에서 바꾼 값이 저장된 것)은 그대로 두고, `--reset-config`일 때만 백업 후 프로필 값으로 되돌림. `default`는 넣지 않음(없는 것만 기본값) |
| 공통 패키지 | python3, openssh-server, util-linux, curl (없을 때만 apt) |
| 워커 시스템 설치 | 모든 워커 폴더의 `workers/<워커>/workspace/install.sh`(udev, 패키지, SDK). 워커를 등록하지 않음(워커 = 워커 폴더, 설정은 그 폴더의 `config.json`·`presets/`) |
| mavlink-router | 서비스 `workspace-mavlink-router`. FC 포트 자동 탐지. 이미 설치되어 있으면 건너뜀 |
| MediaMTX | 서비스 `workspace-mediamtx`. 원격 중계용 ffmpeg. 이미 설치되어 있으면 건너뜀 |
| 서비스 | `workspace.service`(부팅 시 `workspace-start.sh` 실행) |

- 시스템 폴더(/etc 등)에는 서비스 파일 3개만 남깁니다. 실행 파일은 워크스페이스, 설정은 데이터 폴더에 있습니다
- 다시 실행해도 됩니다. 워크스페이스 폴더를 옮겼으면 다시 실행합니다
- 인터넷이 없으면 ffmpeg 같은 apt 패키지만 건너뛰고 경고합니다
- 자세한 것: [install/README.md](install/README.md)

### 갱신

받은 폴더에서 `git pull` 뒤 다시 시작합니다. 설치는 시스템 설치가 있는 새 워커를 받았을 때 또는 프로필 값으로 되돌릴 때(`--reset-config`)만 다시 실행합니다.

**워커 설정**은 워커 폴더의 파일입니다(`workers/<워커>/workspace/config.json`, `presets/<이름>.json`). 화면의 워커 설정 창에서 **적용**(저장 안 함, 이번 부팅 동안)·**저장**(사용 중 프리셋에)·프리셋 고르기·**새 이름으로 저장**을 하고, 저장하면 보드의 워커 폴더 파일만 바뀝니다. 남기거나 다른 보드·PC와 나눌 설정은 직접 커밋합니다(워크스페이스는 깃에 쓰지 않음, design.md 6.3.1).

```bash
cd ~/dev-workspace && git pull
```

```bash
~/dev-workspace/workspace-start.sh
```

### 보드를 새로 플래시한 뒤

holybro-build-image로 만든 이미지(JetPack 6.2.2, 사용자 `dev`)로 플래시했다면 USB-C로 접속해(`ssh dev@192.168.55.1`) PC의 SSH 키를 등록(`ssh-copy-id`)한 뒤 위 순서를 따릅니다. 이미지 저장소: [holybro-build-image](https://github.com/JunghanShin/holybro-build-image)

## 매일 쓰는 법

**시작·종료:** `workspace-start.sh`는 이미 실행 중이면 먼저 멈추고 다시 시작합니다(다시 시작도 이것 하나). `workspace-stop.sh`는 완전히 끌 때만 씁니다.

```bash
~/dev-workspace/workspace-start.sh
```

```bash
~/dev-workspace/workspace-stop.sh
```

- root가 아니면 sudo를 묻습니다. 코드를 고쳤으면 다시 시작(`workspace-start.sh`)(설치는 다시 할 필요 없음)
- 자동 재시작은 없습니다. Manager가 끝나면 전체가 내려갑니다
- 서비스 없이 터미널에서(디버깅): `~/dev-workspace/workspace-start.sh --foreground`

**설정 바꾸기:**

- 워커 설정(장비 주소, 송출, FC 메시지 주기 등)과 자동 시작: 화면에서. 각 패널의 설정 버튼, 또는 시스템 모니터 워커 목록의 설정 버튼
- 워크스페이스 설정(고정 IP, 포트, FC 라우터, 미디어 서버): SSH로 데이터 폴더 `config/`의 파일을 고친 뒤 다시 시작. 파일별 키와 순서는 [docs/config.md](docs/config.md)

**상태:**

```bash
curl -s localhost:8080/workers | python3 -m json.tool
```

```bash
journalctl -u workspace -n 100
```

**워커 시작·정지·설정:**

```bash
curl -X POST localhost:8080/workers/<id>/start
```

```bash
curl -X POST localhost:8080/workers/<id>/stop
```

```bash
curl -X PUT localhost:8080/workers/<id>/config -H 'Content-Type: application/json' -d '{...설정 전체...}'
```

**워커 명령(장치 제어):**

```bash
curl -X POST localhost:8080/workers/<id>/command -H 'Content-Type: application/json' -d '{"cmd": "status", "args": {}}'
```

명령 목록은 각 워커 README에 있습니다. 화면에서도 같은 일을 할 수 있습니다.

| REST 경로 | 내용 |
|---|---|
| `GET /system`, `GET /health` | Manager 클라이언트 접속, 상태 확인 |
| `GET /workers`, `GET /workers/<id>` | 워커 목록·상태·지표 |
| `GET /workers/<id>/config`, `PUT` / `POST …/config/apply` | 설정 읽기, 저장(사용 중 프리셋) / 적용만(임시) |
| `POST /workers/<id>/presets`, `…/presets/<이름>/use`, `PUT·DELETE …/presets/<이름>` | 새 이름으로 저장, 프리셋 고르기, 이름 바꾸기·삭제 |
| `/system/product-build` (`GET`, `…/check`, `POST`, `DELETE`, `…/download`) | 화면 제품 빌드: 상태, 점검, 시작, 취소, 내려받기(design.md 11.4) |
| `PUT /workers/<id>/autostart` | 부팅 시 자동 시작 `{"autostart": true}` |
| `POST /workers/<id>/start`, `/stop` | 시작·정지 |
| `POST /workers/<id>/command` | 워커 명령 `{"cmd", "args"}` |
| `GET /logs/workers/<id>`, `/logs/service`, `/records/commands`, `/history/<id>` | 로그, 명령 이력, 지난 지표 |

## 프로필

프로필은 **프로젝트·현장마다 다른 워크스페이스 설정 파일을 모은 폴더**입니다(망, 포트, 라우터, 미디어 서버). 워커 설정은 프로필이 아니라 워커 폴더의 프리셋에 있습니다. `install/profiles/<이름>/`에 둡니다(예: `myproj`). 설치할 때 이름을 고르고, Enter는 `default`입니다. `default`는 프로필을 넣지 않고 `install/config/`의 기본값을 씁니다. 기본값은 어디서나 동작하는 일반 값입니다(워커 없음. 시스템 모니터는 기본 탑재. 고정 IP 안 씀, 원격 서버 없음).

```text
install/profiles/<이름>/
├─ network.conf          보드 고정 IP
├─ workspace.env         화면 제목(WORKSPACE_UI_TITLE), REST 포트, 영상 주소
├─ mavlink-router.conf   FC 포트, 원격 서버 출구
├─ mediamtx.yml          원격 미디어 서버 중계
├─ manager.json          Manager 설정
└─ README.md             현장 메모(장비 IP, 서버 주소 등). 설치할 때 복사되지 않음
```

파일마다 쓸 수 있는 키는 [docs/config.md](docs/config.md)에 있습니다.

### 프로필이 들어가는 규칙

- **들어 있는 파일만 씁니다.** 바꿀 파일만 넣으면 되고, 없는 파일은 기본값(`install/config/`)을 씁니다. 예: 고정 IP와 화면 제목만 다르면 `network.conf`와 `workspace.env` 두 개
- **처음 설치할 때만 들어갑니다.** 보드에 이미 그 파일이 있으면(화면에서 바꾼 값 포함) 그대로 두고 "보드 값 유지"로 알립니다. 프로필 값으로 되돌리려면 `--reset-config`(기존 파일은 `config/backup-<시각>/`에 남음)
- **저장소에 올라갑니다.** 비밀번호·토큰·키는 넣지 않습니다(IP·주소는 괜찮음, 저장소가 비공개)

### 새 프로필 만들기 — 방법 1: 보드에서 맞춘 뒤 가져오기 (추천)

화면에서 직접 맞춰 보고 잘 되는 값을 그대로 프로필로 만듭니다. 손으로 키를 쓸 필요가 없습니다. 아래 예는 프로필 이름 `myproj`, 보드 주소 `<보드 주소>`, 보드의 워크스페이스 `~/dev-workspace`입니다.

1. **보드에 기본값으로 설치합니다.** 받기·배포 키는 위 "설치 (Jetson)" 1~3번
   ```bash
   sudo bash ~/dev-workspace/install/install_workspace.sh --profile default
   ```
   ```bash
   ~/dev-workspace/workspace-start.sh
   ```
2. **보드에서 설정을 맞춥니다.**
   - 워커 설정·자동 시작: 화면(`http://<보드 주소>:8080`)의 패널 설정 버튼 또는 시스템 모니터 워커 목록의 설정 버튼. 저장하면 워커 폴더 프리셋에 기록됩니다(남길 설정은 직접 커밋, 프로필과 별개)
   - 워크스페이스 설정(고정 IP, 화면 제목, FC 라우터, 미디어 서버 중계): [docs/config.md](docs/config.md)대로 `~/dev-workspace_data/config/`의 파일을 고치고 다시 시작
   - 영상·FC·장비가 다 잘 되는지 확인합니다
3. **보드에서 기본값과 다른 파일을 찾습니다.** 출력된 파일만 프로필에 넣으면 됩니다
   ```bash
   cd ~/dev-workspace_data/config && for f in network.conf manager.json workspace.env mavlink-router.conf mediamtx.yml; do [ -f "$f" ] && ! cmp -s "$f" ~/dev-workspace/install/config/"$f" && echo "$f"; done
   ```
4. **PC의 워크스페이스 저장소에서 프로필 폴더를 만들고 그 파일을 가져옵니다.** 3번에서 나온 파일마다 `scp` 한 줄
   ```bash
   mkdir -p install/profiles/myproj
   ```
   ```bash
   scp dev@<보드 주소>:~/dev-workspace_data/config/network.conf install/profiles/myproj/
   ```
5. **현장 메모를 씁니다.** `install/profiles/myproj/README.md`에 나중에 볼 값: 보드 IP, 장비 IP, 서버 주소, 접속 방법, 이 보드만의 특이 사항.
6. **검사를 돌립니다.** 워커 프리셋이 스키마에 맞는지 등을 봅니다
   ```bash
   dev/check.sh --quick
   ```
7. **커밋하고 푸시합니다.**
   ```bash
   git add install/profiles/myproj && git commit -m "프로필 myproj" && git push
   ```

### 새 프로필 만들기 — 방법 2: 손으로 쓰기

보드 없이 미리 만들 때입니다.

1. 폴더를 만들고, 바꿀 파일만 기본값에서 복사합니다(기본값 파일에 주석으로 설명이 있음)
   ```bash
   mkdir -p install/profiles/myproj
   ```
   ```bash
   cp install/config/network.conf install/config/workspace.env install/profiles/myproj/
   ```
2. 복사한 파일을 고칩니다. 키는 [docs/config.md](docs/config.md)
3. 위 방법 1의 5~7번(현장 메모, 검사, 커밋)

### 프로필로 설치하기

- **새 보드:** 위 "설치 (Jetson)" 순서에서 설치할 때 프로필 이름을 고릅니다(묻지 않으려면 `--profile myproj`)
  ```bash
  sudo bash ~/dev-workspace/install/install_workspace.sh --profile myproj
  ```
  ```bash
  ~/dev-workspace/workspace-start.sh
  ```
- **이미 설치된 보드에 프로필을 새로 넣거나 프로필 값으로 되돌릴 때:** 받은 뒤 `--reset-config`로 설치하고 다시 시작합니다. 보드에서 바꿨던 값은 `config/backup-<시각>/`에 남습니다
  ```bash
  cd ~/dev-workspace && git pull
  ```
  ```bash
  sudo bash ~/dev-workspace/install/install_workspace.sh --profile myproj --reset-config
  ```
  ```bash
  ~/dev-workspace/workspace-start.sh
  ```
- **확인:** 설치 출력의 `profile:` 줄과 넣은 파일 목록, 화면 제목·워커 목록. 보드 설정 파일과 프로필이 같은지
  ```bash
  diff ~/dev-workspace/install/profiles/myproj/network.conf ~/dev-workspace_data/config/network.conf
  ```
- **PC 개발 실행:** `dev/workspace-dev.sh --profile myproj`

### 프로필 고치기

보드에서 값을 바꿔 잘 되면 그 값을 프로필에도 남깁니다. 방법 1의 3번(다른 파일 찾기, 이번에는 프로필과 비교) → 4번(가져오기) → 6~7번(검사, 커밋)입니다. 프로필과 비교할 때는 3번 명령의 `~/dev-workspace/install/config/`를 `~/dev-workspace/install/profiles/myproj/`로 바꿉니다. 프로필만 고치고 보드에 적용하려면 위 "이미 설치된 보드에 프로필을 새로 넣거나 되돌릴 때"를 따릅니다.

## 데이터 폴더

워크스페이스 옆 `<워크스페이스 폴더>_data/`입니다. 처음 설치·시작할 때 만들어지고, 지우면 다음 시작 때 기본값으로 다시 만듭니다.

| 경로 | 내용 |
|---|---|
| `config/workspace.env` | REST 포트, 화면 제목·영상 주소, 로그 레벨, 기록 주기·용량 |
| `config/network.conf` | 고정 IP |
| `config/mavlink-router.conf`, `config/mediamtx.yml` | 라우터, 미디어 서버 설정 |
| `state/logs/<id>.log` | 워커 로그 |
| `run/temp_config.json` | 저장하지 않고 적용한 워커 설정(이번 부팅 동안) |
| `records/*.jsonl` | 상태·지표·명령 기록 |
| `sdk/` | 워커 SDK(설치할 때 풂) |

바꾸는 법과 적용 방법은 [install/README.md](install/README.md) "데이터 폴더".

## 화면과 영상

**화면:** 보드 REST가 화면을 직접 제공합니다. 보드와 같은 망에 붙은 PC는 브라우저로 `http://<보드 주소>:8080`만 열면 됩니다(멀리 있으면 VPN 등으로 같은 망에 먼저 잇습니다. 워크스페이스 밖의 일). 자세한 것은 [ui/README.md](ui/README.md). 화면 코드는 `ui/web` 하나이고, 워커마다 **전용 패널**을 만듭니다. 잘 만든 패널은 그 장비의 별도 UI로 떼어 쓸 수 있습니다.

**공용 화면:** 시스템 모니터(보드 상태와 추이, 전체 워커), 로그, 명령 이력, 워크스페이스(제품 빌드·적용 프로필·설정 파일). 시스템 모니터의 워커 목록은 워커마다 CPU·메모리·프로세스 수·스레드 수(자식 프로세스 포함)를 보여 주고, 줄을 펼치면 그 워커가 보드를 어떻게 쓰는지 나옵니다: 보드 추이와 같은 기간(실시간, 지난 1·6·24시간)의 CPU·메모리·지표 그래프, 프로세스와 스레드별 CPU(libworker 스레드 이름 rx·tx 등). 워커를 켜고 끌 때 보드와 다른 워커가 어떻게 변하는지 비교하는 데 씁니다.

**영상:** 워커는 영상을 보드 안 MediaMTX(`rtsp://127.0.0.1:8554/<경로>`)로 한 번만 보냅니다. 화면은 보드의 WebRTC(:8889)로 보여 줍니다. 원격 미디어 서버로 중계하려면 프로필의 `mediamtx.yml`에서 켭니다.

자세한 것: [docs/local-ui-media.md](docs/local-ui-media.md)

## 워커 개발

### 워커 폴더 규칙

워커 하나 = 기능 단위 하나(모터, 카메라, 데이터 처리 등) = `workers/<장비>/` 폴더 하나. 안은 스레드·자식 프로세스·모듈 여럿으로 자유롭게 설계합니다. 틀은 [workers/_template](workers/_template/README.md)이고, 새 워커는 `dev/new-worker.sh <폴더>`로 만듭니다(그대로 빌드되고 워크스페이스에 붙는 예제 워커가 나옴: 모듈 source·processor·peer). 단계는 보드(서브시스템) > 워커 > 모듈 > 유닛(design.md 3장 "단계"), 워커는 처음부터 세 층으로 나눕니다(design.md 6.6.4):

| 층 | 위치 | 워크스페이스 밖에서 |
|---|---|---|
| ① 기능 코어 | `lib/<모듈>/core/` | 이것만 다른 프로그램에 복사해 씀(libworker 없음) |
| ② 워커 실행 | `lib/<모듈>/`(모듈), `lib/libworker`, `src/main.cpp` | 독립 실행 파일 |
| ③ 워크스페이스 연동 | `workspace/`(type·설정 스키마·`prepare.sh`·`install.sh`·`bin/`·`panel/`), libworker 빌드 옵션 `WORKER_WORKSPACE` | `workspace/` 삭제, `-DWORKER_WORKSPACE=OFF` |

- 워커 폴더를 넣고 다시 시작하면 붙습니다(등록 단계 없음). 시스템 설치(`workspace/install.sh`)가 있으면 `install_workspace.sh`를 먼저. 빼려면 폴더를 지우고 다시 시작
- 워커 설정은 워커 폴더의 `workspace/config.json`(사용 중 프리셋, 자동 시작)과 `presets/<이름>.json`. 화면에서 바꾸고 깃으로 공유
- 커밋 전에 `dev/check.sh`(Python 시험 + 템플릿·모든 워커 PC 빌드·시험), 보드에서는 `dev/system-check.sh`(실행 중인 워크스페이스 전체 점검)
- 워커끼리는 명령(요청·응답)·스트림(구독)·데이터 버스(큰 데이터)로만 주고받고, 상대는 설정 칸(`worker_id`)으로 정합니다(design.md 6.6.7, 본보기 틀 `lib/peer`)
- 다른 워커를 시작·정지해야 하는 워커(VSM 개발 등)는 워커 type에 `"manager_api": true`를 선언합니다(design.md 6.5). 워크스페이스는 개발 작업대라 운용 모드·제어권이 없습니다(6.1)
- 워커는 호스트 + 모듈(design.md 6.6.3)과 병렬 파이프라인(6.6.1)으로 만듭니다. 만드는 순서: 틀 README "새 워커 만들기", "모듈 만들기·붙이기"
- Manager·REST는 고치지 않습니다. 워커의 패널은 워커 폴더 `workspace/panel/`에 두면 화면이 읽습니다(공용 패널만 `ui/web/panels/`)
- 워커는 모두 이 구성입니다. 예전 구성(폴더 맨 위 `worker-types.json`·`prepare.sh`·`install/`·`bin/`, 패널은 `ui/web/panels/`)도 계속 읽습니다(다른 곳에서 가져온 예전 워커)
- 규칙 전체: [install/README.md](install/README.md) "워커 폴더 추가 규칙"

### C/C++ 워커

PC에서 크로스 빌드합니다([PC 개발 환경](#pc-개발-환경)). 검증한 실행 파일은 저장소의 `workers/<워커>/workspace/bin/`(예전 워커는 `bin/`)에 넣어 두므로, 새 보드는 받아서 설치만 하면 빌드 없이 돕니다.

| 단계 | 할 일 | 보드에서 쓰는 실행 파일 |
|---|---|---|
| 개발·시험 | `jetson-build --test` → `jetson-deploy`로 보드의 `build/`에 복사 | `workers/<워커>/build/` (git에 안 들어감, `bin/`보다 우선) |
| 확정 | 소스 커밋 → `dev/release-worker.sh <워커>` → `workspace/bin/` 커밋·푸시 | `workers/<워커>/workspace/bin/` |
| 확정본으로 되돌리기 | 보드에서 `build/` 폴더를 지우고 다시 시작(`workspace-start.sh`) | `workspace/bin/` |

```bash
cd ~/dev-workspace/workers/<워커>
```

```bash
jetson-build --test
```

```bash
jetson-deploy user@<보드 주소> /home/user/dev-workspace/workers/<워커>/build
```

| 명령 | 내용 |
|---|---|
| `jetson-build` | Jetson(aarch64)용 빌드. 결과는 `build/jetson/install/bin/` |
| `jetson-build --test` | 빌드 후 단위 시험(QEMU로 ARM64 실행) |
| `jetson-build --native --debug --test` | PC용 디버그 빌드와 시험 |
| `jetson-deploy HOST DIR` | 실행 파일을 보드에 복사 |
| 가짜 장치 모드 | 장비 없이 개발: 화면 설정 창에서 워커 프리셋 `sim`(워커의 `tools/<이름>-sim` 또는 가짜 SDK 라이브러리). 워커 목록에 "가상" 표시(design.md 6.6.8) |
| `dev/build-product.sh <프로필>` | 제품 묶음: 개발 부품을 걷어낸 결과물 하나(확정 워커 + 컴파일한 Manager 등 + 잠근 설정 + 기본 화면 + 설치 스크립트). 화면에서는 설정 > 워크스페이스 [제품 빌드](보드가 빌드, 내려받기). `--check`는 점검만. 장비에서 `sudo bash install.sh --owner <계정>`(`--rollback`). 프로필의 `product.json`, design.md 11.4 |
| `dev/deploy-worker.sh <워커>` | 개발 중 빠른 반영: Jetson 빌드 → 보드 `build/` → 그 워커만 다시 시작(워크스페이스 재시작·sudo 없음, `--revert`로 확정 복귀). 보드 접속은 `dev/board.env`(`board.env.example` 복사) |
| `dev/new-worker.sh [--video] <폴더> [type]` | 워커 틀로 새 워커 만들기(예제 워커를 이름만 바꿔 복사, 모듈 이름은 그대로) |
| `dev/system-check.sh [주소]` | 보드에서 워크스페이스 전체 점검(워커 상태·heartbeat·링크·상대 워커·스트림·버전·깃). FAIL이면 종료 코드 1 |
| `dev/release-worker.sh <워커>` | 빌드 결과를 `workspace/bin/`(예전 워커는 `bin/`)에 넣고 `BUILD` 작성(소스에 커밋 안 된 변경이 있으면 멈춤) |

새 C/C++ 워커는 `workers/_template/`을 복사해 시작합니다(표준 구성과 공통 뼈대 사본 `lib/libworker`). 실행 파일에 CMake `install` 규칙을 넣습니다.

### Python 워커·구성 요소

Manager, REST, logger, Python 워커는 빌드 없이 바로 실행됩니다. 고친 뒤 보드에 반영하고 다시 시작(`workspace-start.sh`).

### PC에서 실행

Jetson 없이 PC(WSL)에서 Manager + REST + logger와 PC에서 돌 수 있는 워커를 띄웁니다. 화면도 `http://localhost:8080`으로 열립니다.

```bash
~/dev-workspace/dev/workspace-dev.sh --profile <프로필 이름>
```

### 이름 규칙

| 대상 | 형식 | 예 |
|---|---|---|
| 라이브러리 | `lib<대상>` | `libsiyi` |
| 워커 실행 파일 | `<대상>-worker` | `siyi-worker` |
| 도구 | `<대상>-<용도>` | `siyi-cli` |
| C++ namespace | `<대상>` | `siyi` |

## PC 개발 환경

C/C++ 코드는 PC에서 Jetson용으로 크로스 컴파일합니다. 빌드 환경은 **[jetson-dev-env](https://github.com/JunghanShin/jetson-dev-env)** 저장소에 있습니다.

1. WSL2(Ubuntu 22.04)와 Docker 설치
2. 환경 받기·설치
   ```bash
   git clone https://github.com/JunghanShin/jetson-dev-env.git ~/jetson-dev-env
   ```
   ```bash
   ~/jetson-dev-env/install.sh
   ```
3. 이 저장소 받기
   ```bash
   git clone https://github.com/JunghanShin/dev-workspace.git ~/dev-workspace
   ```

두 저장소(환경, 워크스페이스) 모두 비공개라 저장소 소유자에게 협업자 초대를 받아야 합니다. 이 저장소가 요구하는 환경 버전은 `.jetson-dev-env`에 적혀 있고, 다르면 빌드할 때 경고가 나옵니다.

## 포트와 주소

| 항목 | 값 |
|---|---|
| REST API·화면 | 보드 TCP `8080` |
| MediaMTX | RTSP `8554`(워커 수신, RTP UDP `8000`·`8001`), SRT UDP `8890`, WebRTC TCP `8889`·UDP `8189`(시청), HLS `8888` |
| mavlink-router | 워크스페이스 출구 `127.0.0.1:14551`(UDP). 원격 서버 출구는 프로필 |
| USB-C 연결 | 보드 `192.168.55.1` (JetPack 기본) |
| 보드 고정 IP | 프로필의 `network.conf` |

## 문서 안내

| 문서 | 내용 |
|---|---|
| [design.md](design.md) | 설계: 워커 공통 계약, Manager API, REST, 장애 처리, 초기 설정값. 워커별 설계는 `workers/<이름>/design.md` |
| [install/README.md](install/README.md) | 설치·시작·종료, 데이터 폴더, 프로필, 워커 폴더 규칙 |
| [docs/config.md](docs/config.md) | 워크스페이스 설정 파일(고정 IP, 포트, FC 라우터, 미디어 서버): 파일별 키, 바꾸는 순서, 되돌리기 |
| [docs/local-ui-media.md](docs/local-ui-media.md) | 보드에 직접 붙어서 보기: REST 화면 제공, 로컬 MediaMTX, 원격 중계 |
| [docs/fc-mavlink-router.md](docs/fc-mavlink-router.md) | FC(PX4) MAVLink 연동: mavlink-router, FC 파라미터, 확인·문제 해결 |
| [monitor/README.md](monitor/README.md) | 시스템 모니터(기본 탑재 sysmon) |
| [workers/_template/README.md](workers/_template/README.md) | 워커 템플릿: 구성, 모듈 더하기, 시험, 가짜 장치 모드 |
| [workers/replay/README.md](workers/replay/README.md) | 데이터 버스 녹화·재생 워커(개발 도구) |

## 문제 해결

| 증상 | 확인 |
|---|---|
| 화면이 안 열림 | `curl localhost:8080/health`, `journalctl -u workspace -n 50`(REST가 떴는지) |
| 영상이 안 나옴 | 워커 `url`이 `rtsp://127.0.0.1:8554/<경로>`인지, `systemctl status workspace-mediamtx` |
| 워커가 `disabled` | 이 기계에 필요한 도구가 없음(예: PC의 시스템 모니터는 tegrastats 없음). 워커 목록의 마지막 오류 |
| 워커가 `error`(설정 파일) | 워커 폴더 `workspace/config.json`·`presets/`가 JSON으로 읽히는지. 고친 뒤 다시 시작 |
| 워커가 계속 재시작 | `<데이터>/state/logs/<id>.log`, `journalctl -u workspace -n 100` |
| FC 정보가 안 옴 | [docs/fc-mavlink-router.md](docs/fc-mavlink-router.md) "문제 해결" |

## 진행 상황

| 항목 | 상태 |
|---|---|
| Manager, REST, logger, sysmon | Jetson 실기 확인 |
| 공용 영상 송출 모듈(libvideo), 데이터 버스(libframebus) | Jetson 실기 확인(장치 워커로) |
| 보드 화면 제공(REST), 로컬 MediaMTX·원격 중계 | Jetson 실기 확인 |
| FC 연동(mavlink-router) | Jetson·Pixhawk 6X 벤치 확인 |
| 역할 재정의(워커 자체 종료, 워치독) | 설계 확정, 일부 반영 |
| 워커 간 통신(명령·스트림·데이터 버스), Manager API를 쓰는 워커, 템플릿 단계별 시험 | PC 시험 통과(템플릿) |
| VSM 워커(STANAG 4586), C++ 캡처·AI 워커 | 예정 |
