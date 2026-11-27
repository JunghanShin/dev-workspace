# 지도 타일: 보드가 주는 지도 바닥 그림

| 항목 | 내용 |
|---|---|
| 상태 | 구현(2026-11-27, 0.91) |
| 대상 | 화면(워커 패널)이 지도를 그릴 때 쓰는 바닥 타일(256×256 그림). nginx 앞단 `/maps/` |
| 왜 | 화면은 보드에서 받고, 화면을 보는 PC는 인터넷이 없을 수 있다(현장 LAN, LTE는 보드에만). 브라우저가 타일 서버에 바로 가면 지도가 비고, PC마다 같은 타일을 따로 받는다 |
| 바탕 | `docs/gateway.md` 2장(nginx 앞단), `docs/media-proxy.md`(같은 방식으로 앞단에서 넘김) |

## 1. 구조

`GET /maps/<종류>/<z>/<x>/<y>` 하나. nginx가 아래 순서로 찾는다(게이트웨이를 거치지 않음).

| 순서 | 어디서 | 응답 머리 `X-Map-Source` |
|---|---|---|
| ① 오프라인 폴더 | `<데이터>/maps/<종류>/<z>/<x>/<y>.png`(또는 `.jpg`). 운용 지역 타일을 미리 넣어 두면 인터넷 없이 | `offline` |
| ② 캐시 | 한 번 온라인에서 받은 타일(`<데이터>/maps/cache`, 상한 2 GiB, 오래 안 쓴 것부터 지움) | `online HIT` |
| ③ 온라인 | 보드가 타일 서버에서 받아 캐시에 넣고 준다(연결 3 s, 응답 10 s) | `online MISS` |
| ④ 없음 | 위가 모두 안 되면(인터넷 없음, 타일 서버 오류·거절) `204`. 화면은 그 칸을 비워 둔다 | — |

- 종류(기본): `osm` = OpenStreetMap(`https://tile.openstreetmap.org/{z}/{x}/{y}.png`), `satellite` = Esri World Imagery(`…/World_Imagery/MapServer/tile/{z}/{y}/{x}`, 키 필요 없음). 종류 이름만 바꾸지 않으면 화면은 그대로
- 보드가 타일 서버에 갈 때 `User-Agent: workspace-map-tiles/1.0`, 쿠키는 보내지 않음. 이름 풀이는 보드 `/etc/resolv.conf` 첫 nameserver(없으면 오프라인만)
- 로그인을 넣은 제품(`docs/modes.md` 로그인)이면 `/media/`와 같이 로그인한 사람만(`auth_request`). 모듈이 없으면 `/maps/`를 뺀다(열어 두지 않음)
- 화면에는 출처를 늘 표시한다(OSM "© OpenStreetMap contributors", Esri "Imagery © Esri, …"). 타일 이용 조건 때문
- nginx 없이 게이트웨이가 직접 받는 실행(PC 개발 `dev/workspace-dev.sh`)에는 `/maps/`가 없다(404). 화면은 그때 브라우저가 타일 서버에서 바로 받는다(패널 쪽 일)

## 2. 설정

`<데이터>/config/workspace.env`(설정 탭에는 없음, 바꾸면 시작 셸이 다음 시작에 적용):

| 키 | 기본 | 뜻 |
|---|---|---|
| `WORKSPACE_MAPS` | `on` | `off`면 `/maps/` 없음 |
| `WORKSPACE_MAPS_ONLINE` | `on` | `off`면 오프라인 폴더만(보드가 인터넷으로 타일을 받지 않음: LTE 데이터를 쓰면 안 될 때) |

시험·특수 용도(`gateway/nginx/render.sh` 환경): `RENDER_MAPS_SOURCES="<종류>=<주소 틀> …"`(`{z}` `{x}` `{y}`), `RENDER_MAPS_CACHE_MB`.

## 3. 오프라인 타일 넣기

- 폴더 모양: `<데이터>/maps/osm/<z>/<x>/<y>.png`, `<데이터>/maps/satellite/<z>/<x>/<y>.jpg`(XYZ 타일, y는 위에서부터)
- 넣는 즉시 쓴다(다시 시작 필요 없음). 캐시보다 먼저 본다
- **OSM 공용 타일 서버는 대량 내려받기(지역 전체 미리 받기)를 이용 정책으로 막는다.** 오프라인 지도는 그렇게 받지 말고, 쓸 수 있는 원본(직접 만든 타일, 이용이 허락된 지도 서비스, MBTiles를 폴더로 푼 것)에서 만든다. 화면으로 한 번 본 지역이 캐시에 남는 것은 보통 사용
- MBTiles 파일을 바로 읽는 기능은 없다(필요하면 폴더로 푸는 도구를 따로)

## 4. 확인

- 시험 `rest/test_nginx_front.py`: 오프라인 파일 → `offline`, 없는 타일 → 가짜 타일 서버에서 `online MISS` → 다시 `online HIT`(타일 서버에 한 번만 감), 닫힌 서버 → `204`, 안쪽 위치(`/_maps_none`)는 바깥에서 404. 설정 만들기(`Render`): 기본 끔, 종류·주소 틀, 온라인 끔, 로그인 켬(`auth_request`), 모듈 없음
- PC(WSL nginx 1.24, 2026-11-27): 오프라인 파일, OSM `online MISS` → `HIT`, 위성 `online MISS`(JPEG), 없는 줌(25) `204`
- 보드: 2026-11-27 보드에서 OSM·Esri 타일을 받을 수 있음(200, 0.16·0.27 s), nginx ssl 모듈 있음
