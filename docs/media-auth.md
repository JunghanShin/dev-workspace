# 영상 로그인: 로그인을 켜면 영상도 로그인한 사람만

| 항목 | 내용 |
|---|---|
| 상태 | 구현(2026-11-25, 0.86), 켰을 때만 묻기(0.89). 보드 확인(2026-11-26): 끔이면 `authMethod: internal`·바깥 RTSP 허락, 켬이면 `http`·화면 `/workers`·`/media/` 401(토큰 200), 바깥 RTSP 자격 없음 401·토큰 h264, 보드 안 127.0.0.1 그대로 |
| 대상 | nginx `/media/`(화면 영상 틀), MediaMTX 포트를 바로 여는 프로그램(RTSP 8554·WebRTC 8889·HLS 8888·SRT 8890), 게이트웨이 판정 두 곳 |
| 왜 | 로그인(`WORKSPACE_AUTH=on`, 지금은 로그인을 넣은 제품만)을 켜도 영상은 누구나 볼 수 있었다(`docs/review-2026-11-25.md` 6장 2번). `/media/`는 8889를 그대로 넘기고, MediaMTX 기본 설정은 아무 이름이나 받는다 |
| 바탕 | `docs/media-proxy.md`(`/media/`), `docs/gateway.md` 3장(사용자·토큰·역할), `docs/live-settings.md`(로그인 바로 켜고 끄기), MediaMTX v1.21.3 문서 "Authentication"(HTTP-based) |

## 1. 구조

규칙 하나, 판정은 게이트웨이 한 곳, 문 둘.

| 규칙 | 내용 |
|---|---|
| 누가 보나 | 로그인한 사람(세션 쿠키 또는 사용자별 토큰), 역할 `viewer` 이상 |
| 보드 안 | 127.0.0.1·::1은 늘 허락(워커 송출, nginx가 넘긴 요청, 원격 중계 ffmpeg, 탭) |
| 로그인 꺼짐 | 모두 허락(예전과 같음) |
| 바깥 송출 | 로그인 켬이면 늘 거절(`publish`는 보드 안 워커만) |

| 문 | 길 | 판정 |
|---|---|---|
| ① 화면(브라우저) | nginx `/media/` → `auth_request /_media_auth` → 게이트웨이 `GET /api/auth/check` | 쿠키·Bearer로 로그인한 사람이면 `204`(넘김), 아니면 `401`(nginx가 그대로 401). 안쪽 확인은 `proxy_method GET`·본문 없이(WHEP는 POST·PATCH·DELETE) |
| ② 프로그램(바로) | MediaMTX `authMethod: http` → 게이트웨이 보드 안 포트 `POST http://127.0.0.1:8079/api/auth/media` | 본문 `{user, password, token, ip, action, path, protocol, id, query}` → `200` 허락 · `401` 자격 물음 · `403` 거절 |

- ①은 로그인 켬일 때만 nginx 설정에 들어간다(`render.sh` `RENDER_AUTH=on`, `apply.sh`가 시작 셸이 정한 `WORKSPACE_AUTH=on`이면 켬). 꺼짐이면 예전과 같은 설정. nginx에 `auth_request` 모듈이 없으면 `/media/`를 빼고 경고(열어 두지 않음). 보드 nginx 1.18은 모듈 있음
- ②는 **로그인을 켰을 때만** 묻는다(0.89): 게이트웨이가 시작할 때와 로그인을 바꿀 때 `<데이터>/config/mediamtx.yml`의 두 줄을 맞춘다. 켬(로그인을 넣은 제품)이면 `authMethod: http`, 끔이면 `authMethod: internal`(MediaMTX 기본: 모두 허락), `authHTTPAddress: http://127.0.0.1:<WORKSPACE_MEDIA_AUTH_PORT>/api/auth/media`. 줄이 없으면 끝에 덧붙이고, 그 밖 줄·주석은 그대로(원자적 쓰기). MediaMTX는 설정 파일이 바뀌면 저절로 다시 읽으므로 서비스 재시작·root가 필요 없다(`docs/live-settings.md`)
  - 왜 끔일 때 묻지 않나: 평소(로그인 끔)에는 영상이 게이트웨이와 무관하게 돌아야 한다. 게이트웨이가 잠깐 멈췄다고 장비 영상이 막히면 고장이 번진다
  - 대가: 로그인을 켜고 끄는 순간 MediaMTX가 설정을 다시 읽으며 RTSP·WebRTC·HLS·SRT 서버를 모두 다시 연다. 보드 실측: 워커 송출이 약 2 s 뒤 다시 붙고, 그 사이 보던 화면·바깥 수신은 끊겼다 다시 붙는다(그 2 s 동안 오는 요청은 404). 로그인은 자주 바꾸는 값이 아님
  - 서비스 파일에는 `MTX_AUTH*` 환경 변수를 두지 않는다(0.87에 잠깐 두었음: 환경 변수는 파일보다 앞서 바꿀 수 없음)
  - 판정 포트가 없으면(`WORKSPACE_MEDIA_AUTH_PORT=0`) 켬이어도 `internal`(물을 곳이 없음, 경고 대신 nginx `/media/`만 막힘)
- ②의 포트는 게이트웨이가 따로 여는 보드 안 전용(127.0.0.1) 포트. 게이트웨이가 nginx 뒤에서 Unix 소켓만 열어도(`WORKSPACE_GATEWAY_TCP=0`), nginx가 없어도 같은 주소. 이 포트에는 `/api/auth/media` 하나만(그 밖 404), 화면 쪽 포트·소켓에는 이 경로가 없다
- nginx가 넘긴 브라우저 요청은 MediaMTX에 127.0.0.1로 보이므로 ②에서 다시 묻지 않는다(①에서 이미 확인)

**②의 자격(MediaMTX가 채우는 칸):**

| 프로그램 | 넣는 곳 | 게이트웨이가 보는 칸 |
|---|---|---|
| RTSP | `rtsp://<이름>:<토큰>@<보드>:8554/<경로>` 또는 `?token=<토큰>` | `password`(= 토큰) 또는 `token` |
| SRT | `streamid=read:<경로>:<아무 이름>:<토큰>` | `password`(= 토큰) |
| WebRTC·HLS | `Authorization: Bearer <토큰>` 또는 Basic `<이름>:<비밀번호>` | `token` 또는 `user`·`password` |

- 토큰: `gateway token add <사용자> <이름>`(지금 것 그대로, `docs/gateway.md` 3장). `token` 칸 먼저, 틀리거나 없으면 `password` 칸을 토큰으로(이름은 아무거나)
- 계정 이름 + 계정 비밀번호도 받는다(PBKDF2라 연결마다 약 0.1~0.3 s)
- 틀리면 주소마다 셈: 5번 틀리면 30 s 동안 그 주소는 거절(화면 로그인과 같은 규칙, 따로 셈). 자격 없이 처음 묻는 것(MediaMTX가 401을 받아야 자격을 보냄)은 세지 않음
- `api`·`metrics`·`pprof`(MediaMTX 관리, 설정에서 꺼져 있음)는 보드 안에서만
- 기록: 거절은 게이트웨이 로그 INFO 한 줄(이름·동작·경로·주소, 비밀번호·토큰은 넣지 않음), 허락은 DEBUG

## 2. 설정

| 키 | 뜻 | 기본 |
|---|---|---|
| `WORKSPACE_AUTH` | 로그인(화면·REST·영상 같이). 시작 셸이 정함(`docs/modes.md` 로그인) | 워크스페이스 늘 `off`, 제품은 빌드 선택(기본 `off`) |
| `WORKSPACE_MEDIA_AUTH_PORT`(게이트웨이 환경) | ② 판정 포트(127.0.0.1) | `8079`, `0`이면 안 엶. `mediamtx.yml`의 `authHTTPAddress`는 게이트웨이가 같이 맞춤 |
| `RENDER_AUTH`(render.sh) | ① `/media/`에 `auth_request` | `apply.sh`가 정함 |

## 3. 시험

| 무엇 | 어떻게 |
|---|---|
| `mediamtx.yml` 두 줄(core) | `test_core` `test_mediamtx_yaml`: 없으면 덧붙임, 켬 http·끔 internal, 같은 값이면 그대로, 포트 0이면 internal, 들여쓴 줄은 안 건드림 |
| 판정(core) | `test_core` `test_media_auth`: 보드 안·꺼짐·켬 × read·playback·publish·api, 역할, 토큰 칸·비밀번호 칸 |
| ② 포트 | `test_rest_accept` `MediaAuth.test_media_port`: 401(자격 없음)·토큰·비밀번호 자리 토큰·계정, 바깥 publish 403, 실패 제한, 설정 탭으로 끄면 바로 모두 허락 |
| ① 확인 | `MediaAuth.test_check`: 204·401, 꺼짐이면 204, POST 405 |
| nginx | `test_nginx_front` `NginxLive`: 켜면 `/media/` 401, Bearer·쿠키면 넘김(WHEP POST·DELETE 포함), `/_media_auth`는 바깥에서 404. `Render.test_media_auth`: 모듈 없으면 `/media/` 뺌 |
| 보드 | ① 로그인 켜고 화면 영상이 나옴(로그아웃하면 영상 틀 401) ② `ffplay rtsp://x:<토큰>@<보드>:8554/<경로>` 됨, 토큰 없으면 거절 ③ 워커 송출·원격 중계가 그대로 ④ MediaMTX 로그에 인증 오류 없음(v1.21.3이 위 JSON 칸·401 흐름 그대로인지) |

## 9. 함께 정할 것 (초안)

| # | 항목 | 초안 |
|---|---|---|
| 1 | 판정 두 곳 | `GET /api/auth/check`(nginx), `POST /api/auth/media`(MediaMTX, 보드 안 포트 8079) |
| 2 | MediaMTX 설정 | 늘 `authMethod: http`(서비스 환경 변수). 로그인을 켜고 꺼도 그대로 |
| 3 | 게이트웨이가 없을 때 | MediaMTX가 판정을 못 받아 새 연결은 모두 거절(이미 열린 연결은 그대로). 시작 셸은 Manager(워커) 다음에 게이트웨이를 띄워 그 사이 1 s 남짓 송출 연결이 거절될 수 있다: 송출 모듈이 다시 붙는다(libvideo 다시 시작). 늘 열어 두는 쪽보다 막는 쪽 |
| 4 | 바깥 송출 | 로그인 켬이면 늘 거절. 꺼짐이면 예전처럼 받음 |
| 5 | 계정 비밀번호 | 받음(토큰을 권함: 빠르고 따로 지울 수 있음) |
| 6 | 다른 주소의 별도 UI | 쿠키가 `SameSite=Strict`라 다른 주소에서 띄운 화면의 `/media/` 틀은 401. 별도 UI는 토큰으로 MediaMTX WebRTC(8889, `Authorization: Bearer`)를 바로 쓰거나 같은 주소로 띄움 |
