# 스트림 묶기: 화면 하나에 스트림 연결 하나

| 항목 | 내용 |
|---|---|
| 상태 | 구현(2026-11-25). 9장 초안 값 그대로. 다른 점은 8장 |
| 대상 | 화면의 실시간 받기(SSE): 게이트웨이 `/workers/{id}/stream`, 화면 `ui/web/system.js`·워커 패널 |
| 왜 | 화면이 워커마다 스트림 연결을 하나씩 연다(건강 띠 2개 + 개요 탭 3개 + 워커 패널 1개 …). 브라우저는 보드 하나에 연결을 6개까지만 열어서, 스트림이 자리를 차지하면 명령·상태 읽기가 줄을 선다. http를 그대로 쓰기로 해 HTTP/2로는 풀지 않는다(`docs/review-2026-11-25.md` 6장 한계 3) |
| 바탕 | design.md 9.7(스트림), `docs/gateway.md`(게이트웨이 스트림 허브 `gw.hub`) |

## 1. 구조

```text
지금:   브라우저 ─ 워커마다 연결 ─▶ /workers/{id}/stream?hz=n      (3~6개)
바꾼 뒤: 브라우저 ─ 연결 하나 ─────▶ /streams?ids=a:2,b:2,c        (워커마다 hz, 없으면 기본)
                                      │ 게이트웨이 허브가 워커 스트림을 하나씩 구독(지금과 같은 허브)
                                      ▼
         한 줄 = 워커 한 줄 + "worker": "<id>"   (모양은 그대로, 이름표만 더함)
```

| 단계 | 하는 일 |
|---|---|
| ① 게이트웨이 `GET /streams?ids=` | 워커 id 목록(쉼표, 각 `id:hz` 가능). 워커마다 허브를 구독해 받은 줄에 `"worker"`를 붙여 한 SSE로 보냄. 없는·꺼진 워커는 그 워커만 `{"worker", "down": true}` 한 줄. 권한·로그인은 지금 스트림과 같음 |
| ② 화면 `streams.js` | `subscribe(id, hz, onLine)` → 끊기(함수). 구독 목록이 바뀌면 연결을 다시 엶(100 ms 모아서). 다시 열면 워커마다 첫 줄이 기준 상태(kf)라 놓치는 것 없음 |
| ③ 쓰는 곳 | `system.js`(매니저 스트림), 워커 패널은 `ctx.stream(id, hz, onLine)`(셸이 넘김). 지금 `new EventSource(...)`를 쓰는 패널은 그대로 동작(옛 길 유지), 바꾸면 연결을 아낌 |

- 옛 길 `/workers/{id}/stream`은 그대로 둔다(바깥 프로그램·옛 패널)
- 탭 하나에 스트림 연결 1개 → 나머지 5개는 명령·읽기에 늘 남음

## 3. 시험

| 무엇 | 어떻게 |
|---|---|
| 게이트웨이 | 두 워커를 묶어 받기(이름표), 하나가 꺼져도 다른 하나는 계속, hz 따로, 권한 |
| 화면 | 구독이 바뀌면 다시 엶, 끊기면 그 워커 줄이 더 안 옴, 연결이 하나뿐인지(브라우저 네트워크) |

## 8. 구현

| 부분 | 파일 |
|---|---|
| 경로·`ids` 읽기·줄 만들기 | `gateway/core/routes.cpp`(`RouteKind::Streams`, `parse_stream_ids`), `core/stream_state.cpp`(`mux_line`) |
| 연결 하나 = 스레드 하나 | `gateway/src/hub.cpp` `MuxSession`: 워커마다 `Subscriber`(합친 상태 하나 = 메모리 상한 그대로), 모두 `Waker` 하나로 깸. 한 step에 보낼 때가 된 워커 줄을 모두 씀. 위쪽 연결(`gw.up.<워커>`)은 허브가 같이 씀 |
| nginx | `location ~ /streams?/?$`(버퍼링 끔, 1 h) |
| 화면 | `ui/web/streams.js`(`subscribe`·`stats`), `system.js`, 셸 `ctx.stream`, 예제 패널 `workers/_template/workspace/panel/panel.js` |
| 시험 | `test_core`(`parse_stream_ids`·`mux_line`·경로·권한), `test_threads`(`MuxSession`: 이름표·워커마다 hz·하나 꺼져도 계속·다시 옴·쓰기 실패면 모두 해제, TSan), `test_rest_accept.StreamsMux`·`AuthOn.test_streams_auth`, `test_nginx_front`(`/streams` 버퍼링 없음) |

설계와 다르거나 더 정한 것:

- 없는 워커도 404가 아니라 down 줄(Manager에 미리 묻지 않음). Manager가 없어도 200에 모두 down. 오류 응답이면 EventSource가 다시 붙지 않아서
- `hz` 인자: `id:hz`가 없는 워커의 기본(없으면 10). 틀린 hz·nan, 빈 목록, 틀린 id, 겹친 id, 33개 이상 → 400
- down 줄은 워커가 꺼진 동안 한 번만. 다시 올라오면 기준 상태(`kf`, `stream: "up"`)부터 그대로
- 화면 `streams.js`: down 줄과 연결 오류를 받는 쪽에 `{worker, down: true, stream: "down"}`로 넘김(옛 `stream: "down"` 처리 그대로 씀). 같은 워커를 여럿이 받으면 hz는 큰 값, 새로 받는 쪽이 생기면 기준 상태를 받게 다시 엶(구독을 끊기만 하고 주소가 같으면 그대로). 응답 오류(401·502 …)로 EventSource가 멈추면 2 s 뒤 다시 엶. 32개를 넘으면 32개씩 연결을 나눔
- 셸은 패널을 내릴 때 패널이 끊지 않은 `ctx.stream` 구독도 끊음

## 9. 함께 정할 것 (초안)

| # | 항목 | 초안 |
|---|---|---|
| 1 | 주소 | `GET /streams?ids=a:2,b` |
| 2 | 한 줄 모양 | 워커 줄 그대로 + `"worker"` |
| 3 | 구독 바꾸기 | 다시 열기(100 ms 모음). 열린 연결에 더하는 길은 만들지 않음 |
| 4 | 옛 길 | 남김 |
