# 설정 바로 적용: 바꾼 칸을 쓰는 곳만 다시 적용

| 항목 | 내용 |
|---|---|
| 상태 | 구현(2026-11-25, 0.86). 9장 초안 값대로. 보드 확인 남음(3장) |
| 대상 | 설정 탭(`PUT /workspace/settings`)의 "다시 시작하면 적용" 칸 일곱: `WORKSPACE_LOG_LEVEL`, `WORKSPACE_HTTPS`·`_PORT`·`_CERT`·`_KEY`, `WORKSPACE_HTTP_REDIRECT`, `WORKSPACE_AUTH` |
| 왜 | 화면에서 바꿔도 워크스페이스를 다시 시작해야 했고, 화면에는 다시 시작이 없었다(`docs/review-2026-11-25.md` 6장 6번). 다시 시작은 워커·매니저까지 내린다 |
| 바탕 | `ui/workspace-screen.md` 2장(설정 탭), `docs/gateway.md` 3장 "설정", `docs/https.md`, `docs/media-auth.md` |

**원칙:** 설정이 바뀌면 그 값을 쓰는 부분만 다시 적용한다. 워크스페이스 전체를 다시 시작하지 않는다. 적용하지 못하면 값은 저장하고 이유를 돌려준다(다음 시작에 쓰임).

## 1. 구조

| 칸 | 쓰는 곳 | 바로 적용 |
|---|---|---|
| 화면 제목·영상 주소 틀·명령 응답 대기 | 게이트웨이 | 그대로(0.84부터) |
| `WORKSPACE_LOG_LEVEL` | 게이트웨이, process-manager | 게이트웨이 `set_log_level`, process-manager op `set_log_level {"level"}`(게이트웨이 전용 소켓 `manager-rest.sock`, `set_access_mode`와 같은 자리). 메모리에만 |
| `WORKSPACE_AUTH` | 게이트웨이 신분 확인, nginx `/media/`, MediaMTX | 게이트웨이가 신분 확인 묶음(AuthOff ↔ AuthOn)을 통째로 바꿔 끼움 + nginx 다시 만들기(`RENDER_AUTH`) + `<데이터>/config/mediamtx.yml`의 `authMethod`(켬 http, 끔 internal)를 바꿔 MediaMTX가 파일을 다시 읽음(서비스 재시작·root 없음, 보던 영상이 잠깐 끊길 수 있음, `docs/media-auth.md` 1장) |
| HTTPS 칸·평문 넘김 | nginx | `gateway/nginx/apply.sh --reload`: 인증서(없으면 자체 서명) → `render.sh` → `nginx -t` → `nginx -s reload` |

- **시작과 같은 스크립트:** 시작 셸의 nginx 설정 만들기(인증서·`RENDER_TLS_*`·render·점검)를 `gateway/nginx/apply.sh`로 옮겼다. 시작 셸은 `--reload` 없이, 게이트웨이는 `--reload`로 부른다. 둘 다 워크스페이스 계정, 둘 다 `<데이터>/config/workspace.env`를 읽는다(환경 변수의 같은 키는 쓰지 않음: 설정 탭에서 지운 칸이 시작 값으로 남지 않게). 그래서 시작과 바로 적용이 늘 같은 설정
- **nginx 다시 읽기:** 새 설정은 `nginx.conf.new`로 만들어 점검하고, 통과하면 바꿔 넣고 `-s reload`(연결을 끊지 않고 새 worker로). 점검에 실패하면 옛 설정 그대로(nginx는 계속), 응답 `failed`에 이유. nginx 프로세스는 그대로(시작 셸의 감시도 그대로)
- **적용됨은 바뀐 뒤에:** `nginx -s reload`는 신호만 보내고 돌아와서, 옛 worker가 QUIT를 받기 전에는 새 연결에 옛 설정으로 답한다(로그인을 켠 직후 인증 없는 요청이 통과할 수 있음). `apply.sh --reload`는 옛 worker가 모두 끝났거나 "shutting down"이 될 때까지(최대 5 s) 기다린 뒤 돌아오므로, 설정 PUT의 `applied`는 새 연결이 새 설정을 받는다는 뜻이다. 이미 열린 스트림은 옛 worker가 끝까지 들고 있다 (CI에서 4번 중 1번 실패한 `test_live_auth_and_https`로 찾음)
- **신분 확인 바꿔 끼우기:** 요청은 시작할 때 지금 묶음을 하나 잡아 끝까지 쓴다(shared_ptr, 바꾸는 잠금 하나, 잠금 안에 I/O 없음). 진행 중인 요청·SSE는 옛 것으로 끝난다. 켤 때 세션은 비어서 시작(다시 로그인). 같은 값이면 그대로(세션 유지)
- **사용자 없이 켜기:** 사용자 파일이 비었으면 켜기를 `400 bad_request`(`fields.WORKSPACE_AUTH`)로 받지 않는다(켜자마자 모두 503이 되어 화면에서 되돌릴 수 없게 되므로). 켠 뒤 파일이 비면 제품 모드처럼 `/health` 말고 503
- **nginx 뒤가 아니면:** (게이트웨이가 TCP로 직접 받음) HTTPS 칸은 저장만 하고 `failed: "needs nginx …"`. 로그인은 게이트웨이에서 적용(이때는 `/media/`도 없음)
- **응답:** `200 {saved, live, restart, applied, failed}`. `applied` = 실제로 적용된 키, `failed` = `{키: 이유}`. `restart`는 늘 빔(형식으로만 남김)
- **GET:** `effective` = 그 값을 쓰는 곳의 지금 값(HTTPS 칸은 nginx가 지금 쓰는 값: 시작 값, 적용에 성공하면 그 값). `pending`은 늘 `false`(화면 계약 모양 그대로)
- 정의(`install/config/workspace-settings.json`)의 칸은 모두 `"apply": "live"`. `apply` 칸과 `restart` 값은 형식으로 남긴다(다시 시작이 필요한 칸이 생기면 쓰도록)

## 3. 시험

| 무엇 | 어떻게 |
|---|---|
| 정의 | `test_rest_accept` `Settings.test_no_restart_fields`: 모든 칸 `live` |
| 로그 수준 | `Settings.test_put_log_level_and_https_live`: 바로 INFO 줄이 남음, process-manager에 `set_log_level`, nginx 없으면 HTTPS `failed "needs nginx"`, `pending` 늘 false |
| 로그인 | `Settings.test_put_auth_live`: 켜면 다음 요청 401, 토큰·로그인은 됨, viewer는 못 끔, 끄면 바로 열림, 다시 켜면 세션 비움. 사용자 없으면 400 |
| nginx | `test_nginx_front` `NginxLive`: 시작 셸처럼 `apply.sh`로 띄우고 PUT으로 로그인(`/media/` 401)·HTTPS 켜기(자체 서명, HTTPS 포트 열림)·못 읽는 인증서(옛 설정 그대로, `failed`)·끄기, nginx pid 그대로 |
| process-manager | `test_manager_accept` `test_set_log_level`: REST 소켓만, 값 검사, 바로 적용 |
| 보드 | ① 설정 탭에서 HTTPS 켜기 → `https://<보드>:8443` 바로, 화면(평문)은 끊기지 않음 ② 로그 수준 DEBUG → `journalctl -u workspace`에 게이트웨이·manager DEBUG 줄 ③ 로그인 켜기 → 다음 요청부터 로그인 화면, 영상 틀도 ④ nginx pid가 그대로(`cat <데이터>/run/nginx/nginx.pid`) |

## 9. 함께 정할 것 (초안)

| # | 항목 | 초안 |
|---|---|---|
| 1 | 원칙 | 바꾼 칸을 쓰는 곳만 다시 적용, 전체 다시 시작 없음 |
| 2 | 적용 스크립트 | `gateway/nginx/apply.sh`(시작 셸과 같이 씀), 상한 20 s |
| 3 | 응답 | `applied`·`failed` 더함, `saved`·`live`·`restart` 그대로 |
| 4 | `pending` | 늘 `false`, 칸은 남김 |
| 5 | 로그인 켤 때 세션 | 비어서 시작 |
| 6 | 사용자 없이 켜기 | 받지 않음(400) |
| 7 | 손으로 고친 `workspace.env` | 지금처럼 다시 시작해야 적용(설정 탭을 거친 것만 바로). 설정 탭이 nginx를 다시 만들 때는 그때의 파일 전체를 읽는다(받는 주소·포트를 손으로 바꿔 두었으면 그것도 같이 적용됨) |
