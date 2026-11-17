# 화면·영상: 보드와 같은 망에서 보기

화면과 영상은 보드(Jetson)가 직접 제공합니다. 보드와 같은 망에 붙은 PC(랜선, USB-C, 또는 VPN 등으로 이은 망)에서 **브라우저만 열면** 됩니다. 망을 잇는 일은 워크스페이스 밖의 일입니다. 영상은 프로필에서 켜면 원격 미디어 서버로도 중계할 수 있습니다.

## 구성

```text
                       ┌─ 보드 ─────────────────────────────────────────────────────────┐
 로컬 PC 브라우저 ──────┼─▶ REST :8080 ── 화면(ui/web) + /api/board·/api/remote ─▶ Manager │
 (랜선 <보드 주소>,     │                                                                │
  USB-C 192.168.55.1)  │   워커 ──RTSP──▶ MediaMTX :8554 ──┬─▶ WebRTC :8889 ─▶ 로컬 브라우저
                       │   (127.0.0.1)                    └─▶ ffmpeg -c copy ─┐  (켰을 때) │
                       └───────────────────────────────────────────────────┼────────────┘
                                                                           ▼
                                                                  원격 미디어 서버 <원격 서버>:8554
                                                                  (WebRTC :8889 / HLS :8888)
```

| 경로 | 로컬 | 원격 |
|---|---|---|
| 화면 | 브라우저 → `http://<보드 주소>:8080`. REST가 화면 파일과 화면용 경로를 직접 제공 | 보드와 같은 망에 이은 뒤 로컬과 같게 |
| 영상 | 브라우저 → 보드 MediaMTX WebRTC `:8889` | 원격 미디어 서버. 보드 MediaMTX가 들어온 경로를 같은 이름으로 중계(**기본은 끔, 프로필에서 켬**) |

- **워커는 영상을 한 번만 보냅니다** (`rtsp://127.0.0.1:8554/<경로>`). 이중 송출은 하지 않습니다
- 중계를 켜면 원격 미디어 서버에 같은 경로 이름으로 영상이 올라갑니다. 다른 곳에서 원격으로 붙어서 볼 수 있습니다
- 원격 링크가 끊겨도 로컬 시청은 계속됩니다. 중계(ffmpeg)는 끝났다가 MediaMTX가 다시 시작합니다
- UI 서버를 따로 두지 않습니다. 보드에는 REST 하나만 돌고 화면도 같은 포트로 나갑니다

## 설치

`install_workspace.sh`가 MediaMTX도 같이 설치합니다(이미 설치되어 있으면 건너뜀). 화면 파일은 워크스페이스의 `ui/`에 들어 있습니다.

MediaMTX만 따로 설치하거나 상태를 볼 때:

```bash
sudo bash ~/dev-workspace/install/install_mediamtx.sh
```

```bash
bash ~/dev-workspace/install/install_mediamtx.sh --check
```

| 항목 | 위치 |
|---|---|
| 압축 파일(MediaMTX v1.21.3 linux arm64, 공식 배포) | `install/mediamtx/mediamtx_v1.21.3_linux_arm64.tar.gz` (sha256 `27f03823…`, 배포 페이지 `checksums.sha256`과 같은 값) |
| 실행 파일 | `install/mediamtx/mediamtx`. 설치할 때 압축 파일에서 풉니다(git에는 넣지 않음, sha256 `a1621ba3…`) |
| 설정 | `<데이터>/config/mediamtx.yml` (기본값 `install/config/mediamtx.yml`, 없을 때만 복사. 프로필에 있으면 프로필 것) |
| 서비스 | `/etc/systemd/system/workspace-mediamtx.service`. 워크스페이스와 독립이고 자동으로 다시 시작 |
| 원격 중계 | ffmpeg (없으면 설치할 때 apt로 받음. 실패해도 로컬 시청은 됨) |
| 화면 파일 | `ui/web`(REST가 제공) |

**이미 설치되어 있으면 건너뜁니다.** 우리 서비스가 아닌 MediaMTX 서비스나 프로세스가 있거나, 8554 포트를 다른 프로그램이 쓰고 있으면 아무것도 바꾸지 않고 알리기만 합니다.

## 쓰는 법

1. PC를 보드에 붙입니다. 랜선이면 같은 망, USB-C면 보드가 `192.168.55.1`(JetPack 기본)
2. 브라우저로 `http://<보드 주소>:8080` (USB-C: `http://192.168.55.1:8080`)
3. 영상 워커의 송출 주소(`url`)를 로컬 MediaMTX(`rtsp://127.0.0.1:8554/<경로>`)로 둡니다. 화면의 송출 설정이나 REST로 바꿉니다

```bash
curl -X PUT localhost:8080/workers/<카메라 워커>/config -H 'Content-Type: application/json' \
  -d '{"streams": [{"name": "main", "url": "rtsp://127.0.0.1:8554/cam", "mode": "encode", "bitrate_kbps": 1500}]}'
```

REST는 정비 모드에서만 켜집니다. 그래서 이 화면도 정비 모드에서만 열립니다.

## 설정

**`<데이터>/config/mediamtx.yml`** (고친 뒤 `sudo systemctl restart workspace-mediamtx`)

| 항목 | 기본값 | 내용 |
|---|---|---|
| `rtspAddress`, `rtpAddress`, `rtcpAddress` | `:8554`, `:8000`, `:8001` | 워커 수신(RTSP, UDP·TCP) |
| `srtAddress` | `:8890` | 워커 수신(SRT, 워커 `transport: srt`) |
| `webrtcAddress`, `webrtcLocalUDPAddress` | `:8889`, `:8189`(UDP) | 로컬 시청(지연 최소) |
| `hlsAddress` | `:8888` | 예비 시청 |
| `paths.all_others` | `{}` (중계 안 함) | 원격 중계를 켜려면 파일 안의 주석 예시(`runOnAvailable: ffmpeg … rtsp://<원격 미디어 서버>:8554/$MTX_PATH`)를 씁니다. **현장 값이라 프로필에 두는 것을 권합니다.** 원격 구간은 UDP, `pkt_size 1200`(VPN MTU 1420에서도 쪼개지지 않게) |
| api, metrics, pprof, playback, rtmp, moq | 끔 | 쓰지 않는 기능 |

중계를 켠 상태에서 특정 경로만 로컬에 두려면 `paths`에 그 경로를 따로 적습니다. 그 경로에는 `all_others`가 적용되지 않습니다.

```yaml
paths:
  cam-0-proc: {}     # 이 경로는 로컬에서만
  all_others:
    runOnAvailable: ...
    runOnAvailableRestart: true
```

**`<데이터>/config/workspace.env`** (고친 뒤 stop → start)

| 키 | 기본값 | 내용 |
|---|---|---|
| `WORKSPACE_UI_MEDIA_URL` | `http://{host}:8889/{path}` | 화면의 영상 주소 틀. `{host}` = 브라우저가 접속한 보드 주소, `{path}` = 워커 송출 경로. 원격 미디어 서버를 보려면 `http://<원격 서버>:8889/{path}` |
| `WORKSPACE_UI_TITLE` | `Workspace` | 화면 제목(왼쪽 위, 브라우저 탭). 프로젝트 이름은 프로필에 둡니다 |

## REST가 더 제공하는 경로

화면 코드(ui/web)가 쓰는 경로를 REST가 제공합니다. 구현은 `rest/workspace_rest/ui.py`.

| 경로 | 내용 |
|---|---|
| `GET /`, `GET /<파일>` | 화면 파일(`ui/web`). 그 폴더 밖은 주지 않음 |
| `* /api/board/<경로>` | 접두어만 떼고 기존 REST 경로 그대로(`/api/board/workers` = `/workers`) |
| `GET /api/remote/<이름>` | 보드 읽기(`worker_log`, `service_log`, `commands`, `processes`, `power_modes`, `metrics_history`) → `{"ok", "lines"}` |
| `GET /api/panels` | `ui/web/panels/*/panel.json` 목록 |
| `GET /api/ui` | 화면 설정(제목, 갱신 주기, 영상 주소) |

## 확인

| 확인 | 명령 | 기대 |
|---|---|---|
| MediaMTX | `systemctl status workspace-mediamtx` | active. 로그에 `[RTSP] started with listeners on :8554` |
| 워커 수신 | `journalctl -u workspace-mediamtx -n 20` | `is publishing to path '<경로>'` (중계를 켰으면 `runOnAvailable command started`) |
| 원격 중계 | 원격 PC에서 `ffprobe rtsp://<원격 서버>:8554/<경로>` | h264 스트림 정보 |
| 화면 | `curl -s localhost:8080/api/ui` | 제목, 영상 주소 |

시험(2026-11-11, Jetson Orin NX): 서비스 설치·자동 시작, 영상 워커 → 로컬 MediaMTX → 같은 망 PC 브라우저(WebRTC) 시청, 원격 미디어 서버 중계(ffprobe로 1280x720 H.264 확인), 송출을 멈추면 중계 정리까지 확인했습니다.

## 문제 해결

| 증상 | 원인·조치 |
|---|---|
| 화면이 안 열림 | 운용 모드면 REST가 꺼져 있음(정비 모드만). `<데이터>/config/mode` |
| 영상 안 나옴, MediaMTX 로그에 `no stream is available on path` | 워커가 로컬 MediaMTX로 보내지 않음. 워커 `url`을 `rtsp://127.0.0.1:8554/<경로>`로 |
| 로컬은 되는데 원격에 안 올라감 | 중계가 꺼져 있음(기본). 켰다면 `which ffmpeg`, 원격 주소, `journalctl -u workspace-mediamtx`의 ffmpeg 경고 |
| WebRTC 연결 안 됨 | PC → 보드 UDP 8189가 막혀 있음(방화벽). HLS `http://<보드>:8888/<경로>`로 확인 |
| 설치가 "already installed outside the workspace" | 다른 MediaMTX나 8554 사용 프로그램이 있음. 그 서버를 쓰거나 끄고 다시 설치 |
