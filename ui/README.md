# 화면 (ui/)

워크스페이스의 프론트엔드입니다. **보드 REST(:8080)가 `web/`을 그대로 제공**하므로 따로 실행할 화면 서버는 없습니다. 보드와 같은 망에 붙은 PC(랜선, USB-C, VPN 등)에서 브라우저로 `http://<보드 주소>:8080`을 엽니다. 망을 잇는 일은 워크스페이스 밖의 일입니다.

```text
브라우저 ──▶ 보드 REST :8080 ── 화면 파일(ui/web) + /api/board·/api/remote·/api/ui·/api/panels ──▶ Manager·워커
```

패널은 `/api/board`(REST 경로 그대로), `/api/remote`(보드 읽기: 로그·명령 이력 등, 게이트웨이 `gateway/src/reads.cpp`), `/api/ui`(제목·갱신 주기·영상 주소), `/api/panels`만 씁니다. 구현은 게이트웨이 `gateway/src/ui.cpp`.

- 화면 제목은 보드 `<데이터>/config/workspace.env`의 `WORKSPACE_UI_TITLE`, 영상 주소 틀은 `WORKSPACE_UI_MEDIA_URL`
- 신분 확인(C++ 게이트웨이, `docs/gateway.md` 3장): 셸이 먼저 `/api/auth/me`를 본다. 켜져 있고 세션이 없으면 패널 대신 로그인 창, 세션이 끝나면(401) 다시 로그인 창. 머리에 운용/개발 모드 배지와 사용자·역할·로그아웃(꺼짐이면 "인증 꺼짐"). 패널은 `state.auth`(`{auth, name, role, mode}`)를 받는다. Python REST(엔드포인트 없음)면 배지 없이 지금처럼
- **건강 띠**(머리, `ui/system-panel.md` 4장): status-manager가 있으면 "시스템 정상·주의·고장"과 fault의 "이상 N". 누르면 패널 "시스템". 매니저 주제는 `web/system.js`가 매니저 스트림으로 받는다(늘 여는 것은 system·fault 둘)
- 운용/개발 모드는 워크스페이스 패널에서 바꾼다(운용 모드에서 막히는 조작은 `docs/command-path.md` 3장)(관리 역할만, 한 번 더 물음)

## 화면

왼쪽 목록 묶음은 플랫폼이 정한 **셋뿐**이다(워커가 새 묶음을 만들지 않음). 순서도 이대로.

| 묶음 | 들어가는 것 |
|---|---|
| 시스템 | 공용 패널(`web/panels/`): 워크스페이스 전체 관리(개요·모니터·로그·명령 이력·설정·연결) |
| 페이로드 | 워커 패널(`workers/<워커>/workspace/panel/`)과 자동 화면: 들어간 모듈(장치 워커) |
| 개발 도구 | 개발할 때만 쓰는 워커 패널(예: `replay`) |

| 왼쪽 목록 | 패널 | 내용 |
|---|---|---|
| 시스템 | `board` | 시스템 모니터(sysmon, 기본 탑재)(jtop처럼): 요약 숫자(CPU·GPU·메모리·최고 온도·전력·네트워크), CPU 코어별 막대, 자원 막대(GPU 클럭·메모리·SWAP·EMC·디스크·팬), 추이 그래프(실시간 3분 또는 지난 1·6·24시간, CSV 내려받기), 프로세스 목록(CPU 순), 시스템(전원 모드 이름·가동 시간), 전용 HW·온도·전력 레일, **전체 워커**(상태·자원·오류, libworker 버전, 상대 워커 "→ id", 시작·정지, 부팅 시 자동 시작) |
| 시스템 | `system` | 패널 "시스템"(`ui/system-panel.md`, 처음 열리는 화면): 매니저 6개를 한눈에. 머리 타일(전체 건강·이상 수·명령 거절·기록), 상태(status 워커별 건강·이유), 이상(fault, 확인 버튼), 명령(mission 큐·거절), 명령 임대(워커 `leases`, 열려 있을 때 5 s마다), 모듈(payload), 기록(record 지표), 설정(config). 없는 매니저는 "설치 안 됨" |
| 페이로드 · 개발 도구 | 워커 패널 | 워커 폴더 `workspace/panel/`의 패널(장치 워커마다 전용). `panel.json`의 `group`은 `페이로드` 또는 `개발 도구`(예: `replay` 녹화·재생) |
| 페이로드 | `auto` | 자기 패널이 없는 워커마다 항목 하나. 워커 계약(`io/self.json`)대로 머리(상태·시작/정지·설정), 지표(단위·설명), `get_status` 모듈별 카드를 저절로 그린다. 워커가 `workspace/panel/`을 가지면 그것이 대신 나온다 |
| 시스템 | `logs`, `commands` | 워커 로그·서비스 로그(필터, 자동 새로 읽기), 명령 이력 |
| 시스템 | `connections` | 연결 그림(워커 = 상자, 입력 연결 = 선: 실선 이어짐·빨간 점선 문제·회색 점선 프리셋, 꺼진 워커는 점선 상자), 입력마다 상대 고르기(자동 또는 그 역할을 내보내는 워커, 저장 = `PUT /connections`, 개발 모드에서만), 명령하는 곳(commanders) 보기. 워크스페이스 design.md 6.6.7.4 |
| 시스템 | `workspace` | 제품 빌드(점검·진행·내려받기 모달, 전원 모드 선택), 이 보드에 적용된 프로필, 설정 파일(기본 파일 + 프로필 파일, 내용·출처), `workspace.env` 키별 값·기본값·설명(읽기 전용), 화면 제공 주소·REST 상태·Manager 클라이언트 |

## 영상과 지연 측정

- 영상은 보드 MediaMTX의 WebRTC(:8889). 주소 틀은 `WORKSPACE_UI_MEDIA_URL`(기본 `http://{host}:8889/{path}`)
- 주소를 바꾸려면 "영상 주소"를 펼쳐 고치고 적용(이 PC 브라우저에 저장). "기본값"은 `WORKSPACE_UI_MEDIA_URL`로 다시 만듭니다
- "영상" 스위치를 끄거나 다른 패널로 가면 영상 받기를 멈춥니다
- 지연 측정: "시각 표시"를 켜면 영상 왼쪽 아래에 보드 시각이 찍힙니다. 시청 PC 시계와 빼면 송출 지연. 보드·PC 시계는 NTP로 맞춥니다

## 패널 추가

패널 하나 = 폴더 하나. 워커의 패널은 **워커 폴더 `workers/<워커>/workspace/panel/`**(id는 `panel.json`의 `"id"`), 워커에 매이지 않은 공용 패널(시스템 모니터, 로그, 워크스페이스 등)은 `web/panels/<이름>/`. 다른 파일은 고치지 않는다. 폴더를 넣고 브라우저를 새로 고치면 왼쪽 목록에 붙는다.

`panel.json`

```json
{"title": "열화상", "group": "페이로드", "order": 35, "icon": "camera", "worker_types": ["thermal"]}
```

| 키 | 내용 |
|---|---|
| `title` | 목록·제목에 쓸 이름 |
| `group` | 왼쪽 목록 묶음: 공용 패널은 `시스템`, 워커 패널은 `페이로드`·`개발 도구` 중 하나(점검 `dev/test_workers.py`). 밖의 이름이면 화면은 워커 패널을 `페이로드`, 공용을 `시스템`에 넣고 콘솔에 경고 |
| `order` | 목록 순서(작을수록 위) |
| `worker_types` | 있으면 보드에 이 type의 워커가 있을 때마다 항목이 하나씩 생긴다(워커 패널). 없으면 항목 하나 |
| `icon` | 왼쪽 메뉴 아이콘. `web/kit.js`의 `ICONS` 이름(없으면 info) |

`panel.js`

```js
export default {
  mount(el, ctx) { /* el에 화면을 만든다. 워커 패널이면 ctx.workerId */ },
  update(state, ctx) { /* 갱신 주기마다. state.workers, state.byId, state.system */ },
  unmount(ctx) { /* 타이머 정리 등(선택) */ },
};
```

`ctx`가 주는 것:

| 이름 | 내용 |
|---|---|
| `api(method, path, body)` | 보드 REST 호출(예: `api("POST", "/workers/replay/start")`) |
| `mergeConfig(id, changes)` | 현재 설정에 바꾼 값만 합쳐 보냄. 값이 `null`이면 그 키를 지움(워커 기본값) |
| `remote(name, params)` | 보드 읽기(정해진 명령만). `worker_log`(id, lines), `commands`(lines), `service_log`(lines), `processes`(lines), `power_modes`, `metrics_history`(id, minutes, points, keys, csv: 보드 `logger/workspace_history.py`) |
| `act(label, fn)` | 명령 실행 + 성공·실패 알림 + 상태 다시 읽기 |
| `history(workerId)` | 최근 3분 지표 기록(그래프용) |
| `h`, `fmt`, `stateBadge`, `spark`, `toast` | 요소 만들기, 숫자 표시, 상태 표시, 작은 그래프, 알림 |
| `ui` | 공통 부품(`web/kit.js`). 아래 "패널 디자인 규칙" |

보드 파일을 새로 읽어야 하면 게이트웨이 `gateway/src/reads.cpp`에 읽기 경로를, `gateway/core/ui_rules.cpp`의 `remote_read_path()`에 이름을 하나 추가한다(정해진 읽기 전용만 유지).

## 패널 디자인 규칙

모든 패널은 같은 모양이어야 한다. 그래서 화면 요소는 **공통 부품(`web/kit.js`, 패널에서는 `ctx.ui`)으로만** 만들고, 스타일은 `web/shell.css`의 "공통 부품" 절에만 둔다. 패널 안에 색·글자 크기를 직접 쓰지 않는다. 새 모양이 필요하면 패널이 아니라 `kit.js`·`shell.css`에 부품을 추가한다.

**원칙**
- 글자보다 아이콘: 버튼은 `iconButton`(이름은 `title`, 마우스를 올리면 보임). 글자가 꼭 필요한 동작(저장, 재시작 등)만 `label`
- 글자 단계로 위계: 로고 20, 화면 제목 22, 큰 숫자 26(`readout`)·24(`tile`), 대화창 제목 16, 본문·메뉴 14, 이름표·블록 제목 12, 메뉴 묶음·보조 11. 굵기는 제목·숫자·메뉴 항목만
- 틀(상단 머리, 왼쪽 메뉴)도 같은 부품을 쓴다: 머리 상태는 `pill`(보드 연결), 메뉴 항목은 아이콘 + 이름(워커는 아래에 패널 이름) + 상태 점
- 상태는 색 배지로: `pill`(ok·warn·bad·info·idle), 워커 상태는 `workerPill`
- 스크롤을 만들지 않는다: 자주 쓰는 것만 화면에, 가끔 쓰는 설정·정비·정보·로그는 머리의 아이콘 → `dialog`(안은 `section`으로 나눔)
- 묶음에는 제목: 오른쪽 열은 `block("제목", ...)`, 본문 판은 `card("제목", ...)`
- 위험한 동작(재시작, 포맷, 설정 저장으로 장비 재시작)은 `confirm`으로 한 번 더 묻는다

**배치**

| 패널 종류 | 배치 |
|---|---|
| 영상이 있는 장비(카메라, 짐벌) | `panelHead` + `.lay-media`: 왼쪽 `media`(16:9), 오른쪽 `.side` 한 열에 `block`들(맨 위는 제목 없는 큰 숫자 `readout` 3개) |
| 상태·목록(시스템 모니터, 이력) | `panelHead` + `tiles(tile...)` + `card(표 또는 kvList)` |
| 도구(로그) | `panelHead`의 actions에 선택·검색·아이콘 버튼, 본문은 내용 하나 |

**머리(`panelHead`)**: 제목(워커 id 또는 화면 이름), 배지(워커 상태, 연결 상태), 오른쪽에 아이콘 버튼. 워커 패널은 끝에 `workerControls`(시작/정지)를 두고 앞과 `.ph-sep`으로 나눈다. 자동 시작처럼 가끔 바꾸는 값은 머리가 아니라 설정 창에 둔다. **워커 설정 창은 `workerSettings`(자동 시작 + 워커 스키마로 만든 설정 폼, 워크스페이스 design.md 6.10) 하나를 쓰고, 패널이 설정 칸을 직접 만들지 않는다.** 이 브라우저에만 저장하는 값·정비 명령처럼 워커 설정이 아닌 묶음만 `before`/`after`로 붙인다. 설정 창을 여는 버튼은 `settingsButton(창, 제목)`으로 만든다.

**제품 모드**(제품 묶음의 보드 화면, 워크스페이스 design.md 11.4): 셸이 `/api/ui`의 `product`로 `setProduct`를 부르면 `workerControls`는 빈 칸, `workerSettings`는 열리지 않는 빈 창, `settingsButton`은 `null`(머리에서 빠지고 `.ph-sep`도 빠짐)이 된다. 패널은 따로 할 일이 없다: 위 부품만 쓰면 제품 화면에서 설정·시작/정지가 저절로 사라지고 장비 조작만 남는다. 진단·설정 패널(로그, 명령 이력, 워크스페이스)은 제품 묶음에 들어가지 않는다(워커에 딸린 패널 `worker_types`만 들어감).

**설정 창**: 맨 위 "워커"(자동 시작), 그다음 보기·설정 섹션(제목 + 한 줄 설명, 저장은 섹션 오른쪽 아래), 맨 아래 "정비"(재시작·포맷 등, `danger`). 자주 바꾸는 값(예: 버튼 감도)은 설정 창이 아니라 오른쪽 열 블록으로 꺼낸다.

**흐리게 막지 않는다**: 지금 못 하는 동작도 버튼을 흐리게 하지 말고, 이름표에 이유를 쓰고(예: "사진 (SD 카드 없음)") 주황 점으로 표시한다. 누르면 워커가 이유를 알려 준다. 장비 연결이 끊긴 동안의 조작 버튼만 예외.

**부품 목록**

| 부품 | 용도 |
|---|---|
| `icon(name)` | 아이콘. 이름은 `kit.js`의 `ICONS` |
| `iconButton({icon, title, onClick, kind, size, label})` | 버튼. `kind`: primary·danger·accent, `size`: lg(방향 버튼 등) |
| `holdButton({icon, title, onStart, onStop})` | 누르는 동안 동작 |
| `tapHoldButton({icon, title, onTap, onHoldStart, onHoldStop})` | 짧게 누르면 미세 조정, 길게 누르면 연속(방향·줌·초점 끝 버튼) |
| `pill(icon, text, kind)`, `workerPill(state)` | 상태 배지 |
| `readout(label, unit)`, `tile(label, unit)`, `tiles(...)` | 큰 숫자. `.set(value, digits)` |
| `slider({min, max, step, left, right, format, onChange})` | 값 슬라이더. 끄는 중에는 상태 갱신이 덮지 않음. `.set(value, max)` |
| `jog({left, right, onDir})` | 가운데로 돌아오는 슬라이더(위치를 모르는 연속 동작: 수동 초점 등) |
| `segmented([{value, icon, title}], onPick)` | 아이콘 묶음 선택. `.set(value)` |
| `dialog(title, ...)`, `section({title, hint, danger}, ...)`, `actions(...)` | 대화창(`.show()`, `.close()`, `.dispose()`는 unmount에서). 안은 섹션 판(제목·한 줄 설명·내용), 위험한 동작은 `danger`, 저장 등은 `actions`(오른쪽 정렬) |
| `toggleRow(label, hint, onChange)`, `autostartRow(ctx, id)` | 설정 창의 켜기·끄기 줄, 워커 자동 시작 줄 |
| `block(title, ...)`, `card(title, ...)` | 제목 달린 묶음(오른쪽 열 / 본문) |
| `kvList(rows)`, `formRows(rows)` | 키·값 목록(`.set(rows)`), 설정 폼 |
| `media({title})` | 16:9 영상 틀. `.setUrl()`, `.setMessage()`, `.onPoint = (xr, yr) => …`(클릭 위치 0~1), unmount에서 `.stop()` |
| `panelHead({title, badges, actions})` | 패널 머리 |
| `workerControls(ctx, id, {autostart})` | 시작/정지 버튼(목록 화면에서만 `autostart: true`로 자동 시작 버튼도). `update(state)`에서 `.update(worker)` |
| `workerSettings(ctx, id, {title, before, after})` | 워커 설정 창: 자동 시작 + 워커 스키마(`config_schema`)로 만든 설정 폼(바뀐 값 표시, 칸별 기본값으로, 되돌리기, 적용, 프리셋. `format: worker_id` 칸은 워커 id 목록을 고를 수 있음). `.show()` `.update(worker)` `.dispose()` |

예: `workers/replay/workspace/panel/panel.js`(워커 패널), `web/panels/board/panel.js`(상태), `web/panels/logs/panel.js`(도구).
