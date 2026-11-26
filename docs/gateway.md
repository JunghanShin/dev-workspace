# 게이트웨이 설계

| 항목 | 내용 |
|---|---|
| 상태 | 확정(2026-11-20, design.md 0.49 6.4). 6장 1~2 구현(0.50), 3 nginx 앞단(0.51): 시작 셸이 띄움, PC·보드 확인(보드: nginx가 포트, 게이트웨이는 소켓만, 화면·API·스트림·패널 정상). 4 신분 확인·역할·운용/정비 모드·명령 기록·Manager 캐시(0.52, 6.2): PC 확인, 보드 확인 남음. 5 남음 |
| 대상 | 플랫폼 블록 게이트웨이(design.md 4.1): 바깥(화면·외부 운용·도구)이 워크스페이스로 들어오는 단 하나의 문 |
| 바꾸는 것 | Python REST(`rest/workspace_rest`, 약 1,600줄)를 **nginx(웹서버) + C++ 게이트웨이**로 나눈다(원칙 10: 상주 블록은 C++). 지금 REST 경로는 그대로 두고 4.1의 새 역할을 더한다 |
| 바탕 | design.md 6.4(REST), 6.11(화면 제공), 9.6·9.7(명령·스트림), 10.3(접근 제어), 원칙 11~17 |

## 1. 하는 일과 품질 목표

**한 줄로:** 바깥 요청을 받아 누구인지 확인하고, 해도 되는 요청이면 알맞은 곳(process-manager·매니저·워커)에 넘기고, 남기고, 답을 돌려준다. 내용을 판단하지 않는다.

| 하는 일 | 누가 | |
|---|---|---|
| 화면 파일 내주기(ui/web, 워커 패널) | nginx | 지금은 REST가 함 |
| HTTPS, 요청 크기·접속 수·속도 제한, 압축 | nginx | 새로 |
| 신분 확인(로그인 세션 · API 토큰)과 역할 | 게이트웨이 | 새로 |
| 모드(원칙 17, 0.79 `docs/modes.md`): 시작할 때 정함(워크스페이스 개발, 제품 운용, `WORKSPACE_ACCESS_MODE`), 바꾸지 않음(`PUT /mode` 405), process-manager에 알림. 운용 모드에서 막는 명령 종류는 `docs/command-path.md` 3장. mission 응답의 `advisory`는 머리 `X-Workspace-Advisory`로 | 게이트웨이 | 새로 |
| 전달: Manager API(워커 관리), 워커 명령, 스트림 중계(SSE), 보드 읽기(로그·기록·프로세스), 계약(`/io`) | 게이트웨이 | 지금과 같음 |
| 명령 기록: 누가·언제·무엇을·결과(쓰기 요청 전부) | 게이트웨이(`gw.log`) | 새로 |
| 화면 상태 요청을 캐시로 답하기: Manager 구독 하나를 유지하며 최신 워커 목록·시스템 상태를 들고 있음 | 게이트웨이 | 새로(Manager 부담 줄임) |
| 운용 명령을 매니저로 보내기 | 게이트웨이 | 0.67: 읽기가 아닌 워커 명령은 역할 `mission`을 내보내는 매니저로 `mission.send`(`managers/mission/design.md` 2장). 그 매니저가 꺼져 있으면 503, 없으면 워커로 바로 |

| 하지 않는 일 | 누가 |
|---|---|
| 해도 되나 판단(모드 전환 허가 등) | 매니저 |
| 기록 보관·검색 | record-manager(그 전까지는 데이터 폴더 파일) |
| 사용자·권한 관리 화면과 저장 | config-manager(그 전까지는 설정 파일을 읽기만) |
| 화면 하나만을 위한 전용 기능 | 만들지 않음: 화면에 필요한 것은 워커·매니저의 명령·주제로 |

| # | 품질 목표 | 기준 | 확인 |
|---|---|---|---|
| ① | 바꿔 끼워도 아무도 모름 | 지금 REST 경로·응답·상태 코드·SSE 모양 그대로(화면·다른 화면 프로젝트 수정 없음) | 지금 `rest/test_stream.py` + 새 수락 시험을 Python 판·C++ 판 모두로 |
| ② | 작다 | 게이트웨이 5 MB 이하 + nginx(지금 REST 19~25 MB) | 보드 실측 |
| ③ | 바깥에 열린 곳은 nginx 하나 | 게이트웨이는 Unix 소켓만 엶 | 포트 점검 |
| ④ | 막히지 않는다 | 느린 화면·느린 워커는 그 요청만 실패, 다른 요청·스트림 영향 없음. 모든 기다림에 상한 | 느린 클라이언트·느린 워커 시험 |
| ⑤ | 누가 무엇을 했는지 남는다 | 쓰기 요청마다 기록 한 줄(사용자, 모드, 경로·명령, 결과 code, 걸린 시간) | 수락 시험 |

## 2. 구조

![게이트웨이 실행 구조](gateway.svg)

**nginx를 앞에 두는 이유:** 바깥 HTTP를 직접 해석하는 일(가장 공격받기 쉬운 곳)을 오래 검증된 웹서버에 맡기고, C++ 게이트웨이는 nginx가 걸러 넘긴 요청만 보드 안에서 받는다. 화면 파일은 nginx가 폴더에서 바로 내주므로 화면을 고쳐도 게이트웨이는 그대로다. 화면 파일에는 `Cache-Control: no-cache`를 붙여(게이트웨이가 줄 때는 `no-store`) 업데이트 뒤 브라우저가 옛 파일을 쓰지 않게 한다(ETag로 확인, 같으면 304).

**nginx 띄우는 방법:** 시스템 nginx 설정을 건드리지 않는다. 시작 셸이 데이터 폴더에 설정 파일을 만들어(`run/nginx/nginx.conf`, 포트·인증서·경로는 프로필 `workspace.env`) 워크스페이스 계정으로 띄운다(포트 1024 이상, 443을 쓰면 그 권한만 줌). nginx가 없으면 지금처럼 게이트웨이가 TCP로 직접 받는 개발용 모드(경고).

| 스레드 | 하는 일 | 대기 상한 | 큐 |
|---|---|---|---|
| `gw.http` × N | 요청 하나: 신분 확인 → 권한 → 전달 → 기록 큐 → 응답 | Manager 연결 1 s·응답 2 s, 워커 명령 `WORKSPACE_REST_COMMAND_TIMEOUT` | 동시 요청 N(넘치면 503) |
| SSE 연결 | 연결 하나에 `gw.http` 하나, 전송 간격 `hz`(최대 50). `/streams`(여러 워커를 한 연결로, `docs/stream-mux.md`)도 연결 하나에 스레드 하나(워커마다 구독을 한 고리에서 돎) | 쓰기 상한(느리면 그 화면만 끊음) | 화면·워커당 최신 값만(주제별로 합침) |
| `gw.up.<워커>` | 워커 스트림 구독(워커당 하나), 최신 상태·최근 이벤트 보관 | 끊기면 1 s마다 다시 | — |
| `gw.mgr` | Manager 구독 하나: 워커 목록·시스템 상태 캐시, Manager의 `access_mode`가 다르면 다시 알림 | 연결 1 s, 읽기 0.5 s씩, 5 s 조용하면 끊고 1 s 뒤 다시(그동안·3 s 넘게 안 바뀐 캐시면 `GET /workers`는 Manager에 직접) | — |
| `gw.log` | 명령 기록 쓰기 | 끝낼 때 남은 줄 2 s | 1024, 넘치면 버리고 셈(`/health?detail=1`) |

**공유 상태가 작다:** 세션(만료 시각), 운용/개발 모드, 캐시, 워커 계약의 명령 종류 표. 잠금 하나로 지키고 잠금 안에서 I/O를 하지 않는다. 요청 처리는 그 밖에서 상태 없이.

**워커 명령 오류 → HTTP(design.md 9.6, 0.86):** `bad_request` 400 · `forbidden` 403 · `busy` 409 · `unavailable` 503 · `timeout` 504 · `internal`·모르는 code 500. 본문은 워커 `error` 그대로(+ mission `advisory`).

**스트림 묶기(`GET /streams?ids=a:2,b`, `docs/stream-mux.md`):** 화면 하나가 워커 스트림 여럿을 SSE 하나로. `ids`는 쉼표, 각 `id:hz`(없으면 `hz` 인자, 그것도 없으면 10, 0.2~50), 32개까지·겹침 없음(어기면 400). 줄 = 워커 줄 그대로 + `"worker"`. 없는·꺼진 워커는 그 워커만 `{"worker", "down": true}` 한 번(Manager에 미리 묻지 않음: Manager가 없어도 200, 모두 down). 권한은 `/workers/{id}/stream`과 같음(읽기). 허브·워커당 위쪽 연결은 같이 씀. 옛 길 `/workers/{id}/stream`은 그대로(바깥 프로그램·옛 패널). nginx는 `/streams`도 버퍼링 끔

**코드:** `gateway/`(C++, CMake). `core/`(경로 표·권한 규칙·세션·기록 줄 만들기·영상 로그인 판정 `media_auth`, 시스템 호출 모름, 단위 시험), `src/`(HTTP: cpp-httplib 사본, Unix 소켓 클라이언트, SSE, 보드 읽기), `lib/`(libworker 일부·nlohmann·cpp-httplib 사본), `nginx/`(설정 틀 `nginx.conf.in`·`render.sh`, 적용 `apply.sh`, 자체 서명 `make-cert.sh`).

## 3. 인터페이스

바꾸지 않는 것: design.md 6.4의 경로·상태 코드 표, 6.11의 화면 경로(`/api/board/…`, `/api/remote/…`, `/api/panels`, `/api/ui`), SSE 메시지 모양(9.7 + `stream`), `GET /workers/{id}/io`.

더하는 것:

경로는 모두 `/api/board` 접두어로도, `/api` 없이(`/auth/login`, `/mode`)도 된다. Python 판에는 없다(404).

| 경로 | 내용 |
|---|---|
| `POST /api/auth/login` `{name, password}` | 맞으면 `200 {auth, name, role, mode}` + 쿠키 `workspace_session`(HttpOnly, SameSite=Strict, Path=/, Max-Age 7일). 틀리면 `401`, 주소마다 5번 틀리면 30 s `429`(+ `Retry-After`) |
| `POST /api/auth/logout` | 세션을 지우고 쿠키를 비움(`200 {}`) |
| `GET /api/auth/me` | `{auth: "on", name, role, mode, via: "session"\|"token"}`, 로그인 전 `401 {error, auth: "on"}`. 인증 꺼짐이면 `{auth: "off", name: null, role: "maintainer", mode}` |
| `GET /api/auth/check` | 영상 `/media/` 로그인 확인(nginx `auth_request`, 0.86, `docs/media-auth.md`): 쿠키·Bearer가 로그인한 사람(viewer 이상)이면 `204`, 아니면 `401`, 본문 없음. 로그인 꺼짐이면 늘 `204`. GET만(다른 메서드 405) |
| `POST /api/auth/media`(보드 안 전용 포트 `127.0.0.1:8079`) | MediaMTX `authMethod: http`가 연결마다 부름(0.86, `docs/media-auth.md`). 본문 `{user, password, token, ip, action, path, protocol, id, query}` → `200` 허락·`401` 자격 물음·`403` 거절. 화면 쪽 포트·소켓에는 없음. 포트 `WORKSPACE_MEDIA_AUTH_PORT`(0이면 안 엶) |
| `Authorization: Bearer <토큰>` | 프로그램(외부 운용·원격 화면)용. GET이면 `?token=`도(EventSource). 틀린 토큰이면 쿠키가 맞아도 익명 |
| `GET /api/mode` · `PUT /api/mode {"mode": "operation"\|"development"}` | `{mode, since, by}`(+ PUT 답에 `manager`: 알림 결과). 바꾸기는 `maintainer`만. 저장하지 않음(시작하면 운용). 바꾸면 기록하고 process-manager에 `set_access_mode` op(design.md 6.2). 알림이 실패하거나 process-manager만 다시 시작되면 `gw.mgr`가 Manager의 `access_mode`를 보고 다시 알린다(모드 주인은 게이트웨이) |
| `GET /health?detail=1` | 기본 답에 `gateway`: 신분 확인·사용자·세션 수, 모드, 명령 기록(`written`·`dropped`·`errors`), 캐시(`fresh`·`age_s`·`direct_lists`) |
| `GET /workspace/settings` | 설정 탭(`ui/workspace-screen.md` 2장). `{groups: [{title, fields: [{<정의 칸>…, value, effective, pending}]}], pending: [키…], file, product}`. 아래 "설정" |
| `PUT /workspace/settings` `{"<키>": 값, ...}` | 검사 후 `<데이터>/config/workspace.env`의 그 줄만 바꾸고 바로 적용(`docs/live-settings.md`). `200 {saved, live, restart, applied, failed}`. `maintainer`, 운용 모드면 `development_required`, 제품이면 `403 forbidden "settings are locked in product"` |

| 역할 | 할 수 있는 것(신분 확인 켜짐일 때) |
|---|---|
| (로그인 전) | `/health`, 화면 파일(`/`, `*.js` 등, 로그인 화면이 떠야 하므로), 로그인·로그아웃·`me` |
| `viewer` | 읽기: GET 전부(상태·스트림·로그·계약 `/io`·패널·`/api/ui`·`/api/remote`·모드 보기) |
| `operator` | 읽기 + 운용 명령(`POST /workers/{id}/command`, 매니저가 생기면 매니저로) |
| `maintainer` | 위 + 나머지 쓰기 전부(시작·정지·설정·프리셋·자동 시작), 제품 빌드(GET 포함), 모드 바꾸기 |

익명은 `401 {"error": {"code": "unauthorized", …}}`, 역할이 모자라면 `403 forbidden`. 규칙은 `core/access.cpp`(`classify`·`allows`).

**모드 검사(0.55, `docs/command-path.md` 3·5장):** 역할 검사 다음에 본다. 운용 모드면 워커 명령 중 읽기·역할 명령(그 워커가 내보내는 `io/<역할>.json`)만 넘기고, 워커 전용·계약에 없는 명령과 GET이 아닌 나머지(시작·정지·설정·프리셋·자동 시작·연결 선언·제품 빌드)는 `403 {"error": {"code": "forbidden", "message": "development mode required"}, "need": "development"}`. 로그인·로그아웃·모드 바꾸기는 모드와 상관없음. 명령 종류는 워커 계약 파일(`GET /workers/{id}/io`와 같은 것)에서 만들고 파일이 바뀌면 다시 만든다. 넘기는 명령에 `from`(`screen`, 토큰이면 `remote:<토큰 이름>`)을 붙이고 명령 기록 줄에도 `from`.

- **사용자 파일:** `<데이터>/config/users.json` `{"users": [{"name", "role", "password_hash", "tokens": [{"name", "hash"}]}]}`, 권한 0600. 비밀번호는 PBKDF2-HMAC-SHA256 `pbkdf2-sha256$<반복>$<소금 base64>$<해시 base64>`(반복 200000, 소금 16바이트, 해시 32바이트, 같은 시간 비교). 토큰은 무작위 32바이트(16진 64자)를 만들 때 한 번만 보여 주고 파일에는 sha256 16진만. 원문은 어디에도 저장하지 않음. 1 s마다 바뀌었는지 보고 다시 읽는다(재시작 없이 반영, 깨진 파일이면 마지막 값). 비밀번호를 바꾸거나 사용자를 지우면 그 사람의 세션이 끝난다. 프로필로 처음 넣거나 아래 명령으로, config-manager가 생기면 그쪽이 관리
- **설정 명령**(보드에서, 비밀번호는 명령 줄로 받지 않음: 터미널이면 안 보이게, 아니면 표준 입력 두 줄): `WORKSPACE_DATA=<데이터> gateway user add <이름> <역할>` · `user passwd <이름>` · `user del <이름>` · `user list` · `token add <사용자> <토큰 이름>`(토큰을 한 번 출력) · `token del <사용자> <토큰 이름>`. 쓰기는 임시 파일 → fsync → rename. `WORKSPACE_AUTH_ITERATIONS`는 시험용(반복 수를 줄임)
- **세션:** 메모리에만(게이트웨이를 다시 시작하면 다시 로그인). 쉼 12시간, 절대 7일. HTTPS가 붙으면 쿠키에 `Secure`를 더한다(3단계 미결)
- **신분 확인 켜고 끄기(0.90, `docs/modes.md` 로그인):** 게이트웨이는 `WORKSPACE_AUTH=on|off`(기본 `off`)를 따르고, 값은 시작 셸이 정한다. 워크스페이스(개발)는 늘 `off`(`workspace.env`에 적어도 읽지 않음, `WORKSPACE_REST_TOKEN` 토큰 검사는 그대로). 제품은 빌드할 때 고른 값(`dev/build_product.py --login on|off`, 화면 빌드 창 "로그인", product.json `"login": true`, 기본 없음 → 묶음의 `PRODUCT_LOGIN`). 0.89까지는 제품이면 늘 켬·설정 탭에서 켜고 끄기였다. 켬인데 사용자 파일이 없거나 비면 `/health` 말고 모두 `503`과 오류 한 줄(조용히 열어 두지 않음). 로그인을 넣은 제품은 설치(`install.sh`)가 첫 계정 `admin`을 만든다. 켜면 영상도 로그인한 사람만(`docs/media-auth.md`). `on`이면 `WORKSPACE_REST_TOKEN`은 쓰지 않는다(사용자별 토큰으로)
- **보낸 쪽 주소:** TCP면 접속 주소, Unix 소켓(nginx 뒤)이면 nginx가 넣는 `X-Forwarded-For`(`$remote_addr`로 덮어씀). 로그인 실패 제한과 명령 기록에 쓴다
- **명령 기록:** GET이 아닌 요청마다(로그인 성공·실패, 로그아웃 포함) 한 줄 `{"t", "user", "role", "mode", "remote", "method", "path", "cmd", "from", "code", "ms"}`, mission이 `advisory`를 붙인 명령은 `advisory`도(0.81). **읽기 명령(`get_status`, 계약의 `read`)은 남기지 않는다**(0.81: 화면이 주기로 읽어 기록의 99% 넘게가 `get_status`였음). `cmd`는 워커 명령 이름, Manager op, `login`·`logout`·`mode:<값>`. `mode`는 요청을 받을 때의 모드. 본문·쿼리·헤더는 넣지 않는다(비밀번호·토큰). 로그인 실패에서 없는 이름은 `?`로(이름 칸에 비밀번호를 넣었을 수 있음). 파일 `<데이터>/records/gateway/gateway-<부팅 id 8자>.jsonl`(0600): logger 기록(`records/*.jsonl`)과 폴더를 나눴다. 같이 두면 logger의 전체 용량 정리가 이 파일도 지운다. 화면 명령 이력(`/records/commands`, 0.81)은 이 기록의 끝에서 읽는다(새 파일부터, 파일마다 끝부분만 거슬러 읽음). 파일 하나 64 MiB를 넘으면 `gateway-<부팅>-<ms>.jsonl`로 바꾸고 새로, 전체 512 MiB를 넘으면 오래된 것부터 지움. record-manager가 생기면 그쪽으로
- **설정(`ui/workspace-screen.md` 2장):** 칸 정의는 `<워크스페이스>/install/config/workspace-settings.json`(요청마다 다시 읽음) `{"version", "groups": [{"title", "fields": [{key, label, type: string|number|integer|enum, min, max, unit, enum, apply: live|restart, default, description}]}]}`. 받는 주소·포트·토큰·화면 켬(`WORKSPACE_REST_BIND`·`_PORT`·`_TOKEN`·`WORKSPACE_UI`)은 화면을 끊을 수 있어 정의에 넣지 않는다
  - `GET`: 칸마다 `value`(파일 값, 형식대로. 없거나 비면 `null`), `effective`(그 값을 쓰는 곳의 지금 값: 게이트웨이 값, HTTPS 칸은 nginx가 지금 쓰는 값 = 시작 값 또는 마지막으로 적용에 성공한 값, 없으면 기본), `pending`(restart 칸의 파일 값이 시작할 때 값과 다름. 0.86부터 restart 칸이 없어 늘 `false`, 칸은 남김). 맨 위 `pending`은 그 키 목록(늘 빔)
  - `PUT`: 키마다 정의대로 검사(정의 밖 키, 형식, 범위, enum). 하나라도 틀리면 아무것도 쓰지 않고 `400 {"error": {"code": "bad_request", "message", "fields": {"<키>": "<이유>"}}}`. 숫자 칸은 숫자 글(`"443"`)도 받는다. 글 칸은 앞뒤 공백을 떼고 `#`·제어 문자·앞뒤 `"`는 막는다(`read_kv`가 `#` 뒤를 버리고 `"`를 떼므로). `null`·`""`은 지움(기본값으로)
  - 파일 고치기(`core/settings.cpp` `env_edit`): 주석·순서·다른 키 그대로. 있는 줄은 그 자리에서 바꿈, `#KEY=` 주석 줄만 있으면 그 줄을 풂, 없으면 끝에 더함. 지우면 `#KEY=옛값`으로(주석 줄이 따로 있으면 그 줄만 남김). 임시 파일 → fsync → rename, 권한 그대로. 명령 기록 `cmd: "settings"`
  - 모든 칸이 live(0.86, `docs/live-settings.md`): 바꾼 칸을 쓰는 곳만 다시 적용. `WORKSPACE_UI_TITLE`·`WORKSPACE_UI_MEDIA_URL`(`/api/ui`), `WORKSPACE_REST_COMMAND_TIMEOUT`(워커 명령 대기), `WORKSPACE_LOG_LEVEL`(게이트웨이 + process-manager op `set_log_level`), `WORKSPACE_AUTH`(신분 확인 바꿔 끼움 + nginx), HTTPS 칸(`WORKSPACE_HTTPS*`·`WORKSPACE_HTTP_REDIRECT`: `gateway/nginx/apply.sh --reload`, 시작 셸과 같은 스크립트, 상한 20 s). nginx 뒤가 아니면 HTTPS 칸은 저장만 하고 `failed: "needs nginx …"`. 적용 못 한 칸은 `failed {키: 이유}`(값은 저장됨, 점검 실패면 nginx는 옛 설정 그대로)
- **Manager 캐시:** `gw.mgr`가 Manager 구독 하나(`workers`·`system`, 1 s 주기)로 최신 값을 들고, `GET /workers`·`/workers/{id}`·`/system`을 3 s 안에 갱신된 캐시로 답한다(Manager `list`·`get`·`system`과 같은 답). 목록 순서는 Manager `list` 순서(처음과 워커 구성이 바뀔 때 `list`를 한 번 불러 배움). 오래됐거나 없는 워커면 지금처럼 직접. 쓰기는 늘 직접. 끄기 `WORKSPACE_GATEWAY_CACHE=0`. 캐시 값은 최대 약 1 s 늦다(`last_heartbeat_ms_ago` 등)

## 4. 끊겼을 때

| 상황 | 게이트웨이 | 화면 |
|---|---|---|
| process-manager 없음 | 관리 요청 503, 캐시는 마지막 값 + `stale` 표시, 1 s마다 다시 붙음 | 워커 목록이 멈춤 표시 |
| 워커 명령 소켓 없음·느림 | 그 요청만 503/504 | 그 버튼만 실패 |
| 워커 스트림 끊김 | 그 워커 SSE에 `{"stream": "down"}`(`/streams`면 `{"worker", "down": true}` 한 번, 다른 워커는 계속), 다시 붙으면 기준 상태 | 그 패널만 "끊김" |
| 화면이 느림 | 그 SSE만 끊음 | 브라우저가 다시 연결 |
| 기록 디스크 느림·가득 | 기록만 버리고 셈(요청은 처리) | 영향 없음 |
| 게이트웨이 죽음 | 시작 셸이 다시 띄움(지금 REST와 같음, 워커·Manager는 계속) | 잠깐 끊김 |
| nginx 죽음 | 시작 셸이 다시 띄움 | 잠깐 끊김 |

## 5. 시험

| 무엇 | 어떻게 |
|---|---|
| 같은 동작(①) | `rest/test_stream.py` + 새 HTTP 수락 시험(모든 경로·상태 코드·SSE 합치기·느린 화면)을 `REST_BIN`으로 Python 판·C++ 판 모두 |
| 신분 확인·권한·모드 | 역할별 허용·거절, 세션 만료, 토큰, 정비 모드 재시작 뒤 운용 |
| 기록 | 쓰기 요청마다 한 줄, 큐 넘침 |
| nginx 앞단 | 화면 파일·`/api` 넘김·SSE 버퍼링 없음·요청 크기 제한 |
| 동시성 | TSan |

## 6. 옮기는 순서

1. HTTP 수락 시험을 지금 Python REST로 먼저 통과
2. C++ 게이트웨이: 지금 기능 그대로(신분 확인 `off`) → 수락 시험 둘 다 통과
3. nginx 앞단(시작 셸이 띄움, 화면 파일·`/api`) → 보드에서 C++로 실측
4. 신분 확인·역할·운용/정비 모드·명령 기록·Manager 캐시
5. 기본을 C++로, Python REST 삭제(0.54, 6.3)

### 6.1 구현 결과 (0.50)

| 항목 | 결과 |
|---|---|
| 수락 시험 | `rest/test_rest_accept.py`(HTTP 블랙박스, 가짜 Manager·워커): Python 판 34(+ `test_stream` 3, 게이트웨이 전용 2개 건너뜀), C++ 판 36(`REST_BIN`) |
| 단위 시험 | `test_core`(경로 표·합치기·지난 지표·비밀값 가림 등), `test_threads`. TSan 경고 0(TSan 판 게이트웨이로 수락 시험도 통과) |
| 자원(PC WSL) | 메모리 20.8 → 6.8 MB(SSE 2개 7.0 MB), 실행 파일 1.4 MB. 같은 요청 묶음 50개/s(명령 3 : 목록 1 : 시스템 1, 가짜 Manager·워커): 요청 하나 CPU 0.50 → 0.21 ms, 전체 2.50 → 1.05% |
| 보드 실측 | 메모리 20.4 → 6.1~7.6 MB, 화면 열린 상태(요청 약 5.5개/s) CPU 0.63%. 고친 것: 재시작 직후 bind 실패(SO_REUSEADDR만), 쉬는 요청 스레드 48개가 1초마다 깨던 것(쉴 때 CPU 0.5%) |
| 실행 | `dev/release-worker.sh gateway` → `gateway/bin/gateway`. 시작 셸: `WORKSPACE_REST_IMPL=cpp` 또는 `<데이터>/config/rest_impl`에 `cpp`(기본 Python). `WORKSPACE_GATEWAY_SOCKET`(Unix 소켓, nginx용), `WORKSPACE_GATEWAY_THREADS`(동시 요청 상한, 기본 48, SSE 포함). 신분 확인은 0.52부터(6.2, 시작 셸이 `workspace.env`의 `WORKSPACE_AUTH`를 넘김) |
| 라이브러리 | cpp-httplib v0.18.3(MIT, 사본) |

**HTTPS:** 설계·설정은 `docs/https.md`(`WORKSPACE_HTTPS`, 기본 꺼짐).

**nginx 앞단(0.51):** `gateway/nginx/nginx.conf.in`(틀)·`render.sh`(시작 셸과 시험이 같이 씀). 시작 셸은 C++ 게이트웨이 + nginx가 있으면 `<데이터>/run/nginx/`에 설정을 만들고 `nginx -t`가 통과할 때만 워크스페이스 계정으로 띄운다(게이트웨이는 `WORKSPACE_GATEWAY_TCP=0`, `run/gateway.sock`). 없거나 실패하면 게이트웨이가 TCP로 직접. 끄기 `WORKSPACE_NGINX=off`. 설치: `sudo apt install nginx-light`, 시스템 nginx 서비스는 끔(`systemctl disable --now nginx`). Ubuntu 22.04 nginx-light(1.18)에는 limit_conn 모듈이 없어 `render.sh`가 그 줄을 뺀다(보드에서 처음 설정 점검 실패 → 게이트웨이가 TCP로 대신 받음, 대비대로). 1.18은 설정을 읽기 전에 기본 error.log를 열어 권한 경고 한 줄을 남긴다(무해). 시험 `rest/test_nginx_front.py`(화면 파일 직접·게이트웨이가 꺼져도 나옴, API·워커 패널은 게이트웨이로, Host 전달, 스트림 버퍼링 없음 1 s 안, 본문 64 KiB 넘으면 413).

Python 판과 다른 점: HEAD·PATCH·OPTIONS는 JSON 501, Range 요청은 206, 워커 명령 시간 상한은 전체 시간으로 셈, Python이 예외로 답 없이 끊던 경우는 500·503으로 답함, 제품 빌드는 취소 중인 자식이 끝나기 전 새 시작을 막음(Python은 옛 상태가 섞였음), SSE 기준 상태를 잠금 안에서 넣어 드문 경합이 없어짐, 새로: 동시 요청 상한(넘으면 503), 받기·스트림 스레드 10 s 멈춤 → exit 4.

### 6.2 구현 결과 (0.52, 4단계)

| 항목 | 결과 |
|---|---|
| 코드 | `core/crypto`(SHA-256·HMAC·PBKDF2·base64, OpenSSL 없이), `core/access`(해시 형식·사용자 파일·역할 표·세션·실패 제한·기록 줄), `src/auth`(AuthOn·사용자 파일 다시 읽기), `src/recorder`(gw.log), `src/mgr_cache`(gw.mgr), `src/users_cli`(설정 명령). process-manager는 C++·Python 판 둘 다 `set_access_mode` |
| PBKDF2 시간 | 200000회 81~83 ms(PC WSL, Ryzen 7 5800X). 보드는 몇 배 느릴 것(로그인 한 번, 보드 실측 남음) |
| 단위 시험 | `test_core` 357 검사(공개 벡터: NIST SHA-256, RFC 4231 HMAC, RFC 7914 11장·RFC 6070 꼴 PBKDF2, RFC 4648 base64 / 해시 형식·같은 시간 비교·사용자 파일·역할 표 전 경로 종류·세션 쉼·절대 만료(가짜 시계)·실패 제한·기록 줄에 비밀 없음), `test_threads` 84 검사(gw.log 큐 상한·회전·전체 상한, 사용자 파일 바꿔 쓰는 중 읽기, 캐시 순서·멈춤 → 직접) |
| 수락 시험 | `test_rest_accept` C++ 52(새 게이트웨이 전용 14: 인증 꺼짐 그대로, 익명 401, 로그인·로그아웃·쿠키, 역할 표(사용자 3 × 경로 16), 토큰·폐기, 사용자 파일 다시 읽기·설정 명령, 기본 반복 수, 실패 제한·X-Forwarded-For, 제품 모드 강제·사용자 없음 503, 모드·재시작·Manager 알림·Manager 없을 때 다시 맞춤, 명령 기록 줄·비밀 없음, 캐시 같은 답·멈춤·오래됨·끔), Python 35(17 건너뜀). `test_nginx_front` 8(nginx 뒤 보낸 쪽 주소). `test_manager_accept`에 `test_access_mode`(두 판) |
| TSan | 게이트웨이·process-manager 단위 시험 경고 0, TSan 판 게이트웨이로 수락 시험 63개(`test_rest_accept`·`test_stream`·`test_nginx_front`) 통과. `dev/check.sh` 전체 통과 |
| 캐시 효과(PC, 워커 5개 heartbeat 0.2 s, 30 s) | 화면 요청 20개/s(`/workers`·`/system`): process-manager 0.27~0.43 → 0.20%, 게이트웨이 0.43~0.50 → 0.33%. 2개/s(화면 하나)에서는 차이가 측정 잡음(0.03%) 안 |
| 화면 | 로그인 창, 머리에 사용자·역할·로그아웃(로그인을 넣은 제품만). 0.79에서 모드 카드 없앰, 0.90에서 "개발"·"인증 꺼짐" 배지 없앰(ui/README.md 배지). 브라우저로 확인(PC) |

Python 판과 다른 점(더한 것): 위 3장 경로 전부, `GET /health?detail=1`, 쓰기 요청 기록 파일. 제품 모드는 신분 확인이 늘 켜져 사용자 파일이 있어야 열린다(Python 판은 `WORKSPACE_REST_TOKEN`).

정한 것: 명령 기록 파일을 `records/` 바로 아래가 아닌 `records/gateway/`에(3장 이유). Manager가 구독을 모르면(`ok`가 아닌 첫 답) 캐시는 쓰지 않고 1 s마다 다시 시도. 모드 바꾸기는 Manager 알림이 실패해도 바뀐 채로 답한다(`manager` 칸에 결과).

남은 위험: 제품 묶음(`build_product.py`·`install.sh`)이 사용자 파일을 아직 만들지 않는다(지금 제품은 Python REST라 영향 없음, C++로 옮길 때 프로필 `users.json` 또는 설치 단계 필요). HTTPS 전에는 쿠키·토큰이 평문(정비망 전제). 세션 표·실패 제한 표는 메모리 상한(4096)만.

### 6.3 구현 결과 (0.54, 5단계: 기본을 C++로, Python REST 삭제)

| 항목 | 결과 |
|---|---|
| 시작 셸 | 게이트웨이만 띄운다(`WORKSPACE_REST_IMPL`·`config/rest_impl` 없어짐). 실행 파일이 없으면 화면·REST 없이 Manager만 돌고 로그에 `dev/release-worker.sh gateway`를 안내. nginx 규칙은 그대로 |
| PC 개발 실행 | `dev/workspace-dev.sh`가 process-manager·게이트웨이를 `~/.cache/workspace-dev/<저장소 폴더>/`에 PC용으로 빌드(바뀐 것만)해 띄운다. 다른 빌드는 `WORKSPACE_MANAGER_BIN`·`WORKSPACE_GATEWAY_BIN`. nginx 없이 게이트웨이가 포트를 직접 |
| 제품 묶음 | `build_product.py`가 C++ 확정 실행 파일(`manager/bin/process-manager`, `gateway/bin/gateway`)과 `gateway/nginx/`를 넣고 심볼을 지운다. Python Manager·REST 컴파일은 뺌(logger·시스템 모니터만 남음). 확정 파일이 없으면 빌드 실패 |
| 제품 첫 계정 | `install.sh`가 사용자가 없으면 `admin`(maintainer, 무작위 비밀번호)과 원격 화면 토큰 `remote`를 만들어 설치 출력에 한 번 보여 준다(옛 `WORKSPACE_REST_TOKEN` 만들기 대신). 7장 두 미결 해결 |
| 지운 것 | `rest/workspace_rest/`(약 1,600줄), `rest/test_stream.py`(같은 SSE 동작은 수락 시험·`test_nginx_front`가 봄) |
| 시험 | `rest/test_rest_accept.py`는 게이트웨이 전용(`REST_BIN` 없으면 건너뜀). 정답 기준: 워크스페이스 정보는 `rest/reference_profile.py`(옛 Python 코드를 시험 기준으로만 남김), 로그 끝은 시험 안 `tail()`, 지난 지표는 logger의 `workspace_history`. 같은 포트 재시작 시험은 게이트웨이 → 게이트웨이 |

## 7. 미결

| 항목 | 언제 |
|---|---|
| HTTPS 인증서를 어디서 받나(보드 자체 서명 또는 프로필). 붙으면 쿠키에 `Secure` | 3단계 |
| 제품 빌드 기능(`/system/product-build`, 개발 전용)을 게이트웨이에 둘지 개발 도구로 뺄지 | 2단계 |
| ~~원격 화면 토큰 받는 방법~~ → 제품은 설치 때 `remote` 토큰(6.3), 개발 보드는 `gateway token add` | 0.54 해결 |
| ~~제품 묶음에 사용자 파일 넣기~~ → 설치 때 첫 계정(6.3) | 0.54 해결 |
