#!/bin/bash
# nginx 설정 만들기(docs/gateway.md 2장): 틀 nginx.conf.in → 실행할 설정. 시작 셸과 시험(rest/test_nginx_front.py)이 같이 쓴다.
#
#   render.sh <출력 파일> <실행 폴더> <게이트웨이 소켓> <받는 주소:포트> <워크스페이스 폴더> [ui=1|0]
#
# 실행 폴더에 임시 폴더(tmp/)를 만든다. mime.types가 시스템에 없으면 최소 표를 만들어 쓴다.
# 깔린 nginx(NGINX_BIN 또는 PATH)에 limit_conn 모듈이 없으면(nginx -V에 --without-http_limit_conn_module,
# 예: Ubuntu 22.04 nginx-light) 주소당 연결 제한을 뺀다. 본문 크기·시간 제한은 그대로.
#
# HTTPS(docs/https.md): 환경 변수로 켠다(없으면 지금과 같음)
#   RENDER_TLS_LISTEN=<주소:포트>  RENDER_TLS_CERT=<인증서>  RENDER_TLS_KEY=<키>
#   RENDER_REDIRECT=on|off(기본 on): 평문 포트로 온 요청을 HTTPS로 넘김(308). 보드 안(127.0.0.1)에서 온 것은 그대로(SSH 터널)
# 영상 넘기기(docs/media-proxy.md): RENDER_MEDIA=<주소:포트>(기본 127.0.0.1:8889, MediaMTX WebRTC), off면 /media/ 없음
# 영상 로그인(docs/media-auth.md): RENDER_AUTH=on이면 /media/에 auth_request(게이트웨이 /api/auth/check). 모듈이 없으면 /media/를 뺀다
# 지도 타일(docs/map-tiles.md): RENDER_MAPS=<폴더>(보통 <데이터>/maps, 없거나 off면 /maps/ 없음)
#   /maps/<종류>/<z>/<x>/<y>: <폴더>/<종류>/<z>/<x>/<y>.png|.jpg(오프라인 타일) → 없으면 온라인에서 받아 <폴더>/cache에 캐시 → 그것도 안 되면 204
#   RENDER_MAPS_ONLINE=on|off(기본 on): off면 오프라인 폴더만. RENDER_MAPS_CACHE_MB(기본 2048): 캐시 상한
#   RENDER_MAPS_SOURCES: "<종류>=<주소 틀> ..."({z} {x} {y}), 기본 osm(OpenStreetMap)·satellite(Esri World Imagery)
#   로그인 켬이면 /media/와 같이 로그인한 사람만(모듈이 없으면 /maps/도 뺀다)
# 이 스크립트를 직접 부르는 곳은 apply.sh(시작 셸·설정 탭 바로 적용이 같이 씀)와 시험
# nginx에 ssl 모듈이 없으면 HTTPS를 빼고(경고), rewrite 모듈이 없으면 넘김만 뺀다.
set -euo pipefail
out="$1" prefix="$2" sock="$3" listen="$4" root="$5" ui="${6:-1}"
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
mkdir -p "$prefix/tmp"
mime=/etc/nginx/mime.types
if [[ ! -f "$mime" ]]; then
  mime="$prefix/mime.types"
  echo 'types { text/html html; text/css css; text/javascript js; application/json json; image/svg+xml svg; image/png png; image/jpeg jpg; }' > "$mime"
fi
nginx_bin="${NGINX_BIN:-$(command -v nginx 2>/dev/null || echo /usr/sbin/nginx)}"
if "$nginx_bin" -V 2>&1 | grep -q -- "--without-http_limit_conn_module"; then
  zone='    # (주소당 연결 제한 없음: 이 nginx에 limit_conn 모듈이 없음)'
  conn=''
else
  zone='    limit_conn_zone $binary_remote_addr zone=perip:1m;'
  conn='        limit_conn perip 64;    # 주소 하나가 연결을 다 쓰지 못하게(화면 여러 개 + 스트림은 넉넉히)'
fi
if [[ "$ui" == 0 ]]; then
  uiloc='        location / { proxy_pass http://gateway; }'
else
  uiloc="        location / {
            root $root/ui/web;
            index shell.html;
            try_files \$uri \$uri/ @gateway;
            add_header Cache-Control \"no-cache\" always;   # 업데이트 뒤 옛 화면 파일을 쓰지 않게(ETag로 확인, 같으면 304)
        }"
fi
tls=''
if [[ -n "${RENDER_TLS_LISTEN:-}" ]]; then
  nv="$("$nginx_bin" -V 2>&1 || true)"
  if ! grep -q -- "--with-http_ssl_module" <<<"$nv"; then
    echo "render.sh: this nginx has no ssl module; HTTPS left out" >&2
    tls='        # (HTTPS 없음: 이 nginx에 ssl 모듈이 없음)'
  else
    tport="${RENDER_TLS_LISTEN##*:}"
    tls="        # HTTPS(docs/https.md): 인증서는 시작 셸이 정함(사용자 것 또는 보드 자체 서명)
        listen $RENDER_TLS_LISTEN ssl;
        ssl_certificate $RENDER_TLS_CERT;
        ssl_certificate_key $RENDER_TLS_KEY;
        ssl_protocols TLSv1.2 TLSv1.3;
        ssl_session_cache shared:tls:1m;
        ssl_session_timeout 1h;"
    if [[ "${RENDER_REDIRECT:-on}" != off ]]; then
      if grep -q -- "--without-http_rewrite_module" <<<"$nv"; then
        echo "render.sh: this nginx has no rewrite module; no HTTP -> HTTPS redirect" >&2
      else
        [[ "$tport" == 443 ]] && to='https://$host' || to="https://\$host:$tport"
        tls+="
        # 평문 포트로 온 요청은 HTTPS로. 보드 안(127.0.0.1, SSH 터널)에서 온 것은 그대로
        set \$to_https \"\";
        if (\$scheme = http) { set \$to_https y; }
        if (\$remote_addr = 127.0.0.1) { set \$to_https \"\"; }
        if (\$to_https) { return 308 $to\$request_uri; }"
      fi
    fi
  fi
fi
media="${RENDER_MEDIA:-127.0.0.1:8889}"
maps="${RENDER_MAPS:-off}"
mediloc='' mapsloc='' mapscache=''
mauth='' mcheck=''
if [[ ( "$media" != off || "$maps" != off ) && "${RENDER_AUTH:-off}" == on ]]; then
  # 로그인 켬(docs/media-auth.md): /media/도 로그인한 사람만. 판정은 게이트웨이(/api/auth/check, 204 들여보냄·401)
  if [[ -z "${nv:-}" ]]; then nv="$("$nginx_bin" -V 2>&1 || true)"; fi
  if ! grep -q -- "--with-http_auth_request_module" <<<"$nv"; then
    echo "render.sh: this nginx has no auth_request module; /media/ and /maps/ left out (login is on)" >&2
    media=off maps=off  # 막을 수 없으면 열어 두지 않는다
  else
    mauth="
            auth_request /_media_auth;  # 로그인 확인(쿠키·Bearer, docs/media-auth.md)"
    mcheck="        # /media/·/maps/ 로그인 확인(auth_request가 부르는 안쪽 요청): 본문 없이 GET으로 게이트웨이에 묻는다(WHEP는 POST·PATCH·DELETE)
        location = /_media_auth {
            internal;
            proxy_pass http://gateway/api/auth/check;
            proxy_method GET;
            proxy_pass_request_body off;
            proxy_set_header Content-Length \"\";
            proxy_set_header Host \$http_host;
            proxy_set_header X-Forwarded-For \$remote_addr;
            proxy_set_header X-Forwarded-Proto \$scheme;
        }"
  fi
fi
if [[ "$media" != off ]]; then
  mediloc="        # 영상(docs/media-proxy.md): 화면과 같은 주소·인증서로 MediaMTX WebRTC 보기 화면과 신호(WHEP)를 넘긴다.
        # 앞의 /media를 떼고 넘김. 영상 데이터는 WebRTC UDP로 바로(여기 안 거침)
        location ^~ /media/ {$mauth
            proxy_pass http://$media/;
            proxy_redirect / /media/;  # WHEP 세션 주소(Location: /<경로>/whep/<id>)도 /media/ 아래로(PATCH·DELETE가 같은 길로)
            absolute_redirect off;     # 상대 그대로(SSH 터널처럼 포트가 달라도 맞게)
            proxy_buffering off;
            proxy_set_header Host \$http_host;
            proxy_read_timeout 60s;
        }"
fi
if [[ "$maps" != off ]]; then
  # 온라인 타일 주소는 변수로 만들므로 이름 풀이(resolver)가 필요하다: 보드의 /etc/resolv.conf 첫 nameserver
  ns="$(awk '$1 == "nameserver" { print $2; exit }' /etc/resolv.conf 2>/dev/null || true)"
  [[ "$ns" == *:* ]] && ns="[$ns]"
  online="${RENDER_MAPS_ONLINE:-on}"
  sources="${RENDER_MAPS_SOURCES:-}"
  [[ -n "$sources" ]] || sources='osm=https://tile.openstreetmap.org/{z}/{x}/{y}.png satellite=https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}'
  [[ -n "$ns" ]] || { [[ "$online" == on ]] && echo "render.sh: no nameserver in /etc/resolv.conf; map tiles offline only" >&2; online=off; }
  cache_mb="${RENDER_MAPS_CACHE_MB:-2048}"
  mapsloc="        # 지도 타일(docs/map-tiles.md): 오프라인 폴더 → 온라인(받아서 캐시) → 없으면 204(화면은 빈 칸)
        location = /_maps_none {
            internal;
            return 204;
        }"
  if [[ "$online" == on ]]; then
    mapscache="    # 지도 타일 캐시(docs/map-tiles.md): 한 번 받은 타일은 보드에 남겨 다시 받지 않는다
    proxy_cache_path $maps/cache levels=1:2 keys_zone=maps:8m max_size=${cache_mb}m inactive=365d use_temp_path=off;"
  fi
  for s in $sources; do
    name="${s%%=*}" url="${s#*=}"
    [[ "$name" =~ ^[a-z0-9_-]+$ && "$url" == http*://* ]] || { echo "render.sh: bad map source: $s" >&2; continue; }
    host="${url#*://}" host="${host%%/*}"
    up="${url//\{z\}/\$mt_z}" up="${up//\{x\}/\$mt_x}" up="${up//\{y\}/\$mt_y}"
    if [[ "$online" == on ]]; then fallback="@maps_$name"; else fallback="/_maps_none"; fi
    mapsloc+="
        location ~ ^/maps/$name/([0-9]+)/([0-9]+)/([0-9]+)\$ {$mauth
            set \$mt_z \$1;
            set \$mt_x \$2;
            set \$mt_y \$3;
            root $maps;
            add_header Cache-Control \"max-age=86400\" always;
            add_header X-Map-Source offline always;
            try_files /$name/\$mt_z/\$mt_x/\$mt_y.png /$name/\$mt_z/\$mt_x/\$mt_y.jpg $fallback;
        }"
    if [[ "$online" == on ]]; then
      mapsloc+="
        location @maps_$name {
            resolver $ns valid=300s ipv6=off;
            resolver_timeout 3s;
            proxy_pass $up;
            proxy_ssl_server_name on;
            proxy_set_header Host $host;
            proxy_set_header User-Agent \"workspace-map-tiles/1.0\";
            proxy_set_header Cookie \"\";
            proxy_hide_header Set-Cookie;
            proxy_ignore_headers Cache-Control Expires Set-Cookie;
            proxy_cache maps;
            proxy_cache_key $name/\$mt_z/\$mt_x/\$mt_y;
            proxy_cache_valid 200 365d;
            proxy_cache_use_stale error timeout updating http_500 http_502 http_503 http_504;
            proxy_cache_lock on;
            proxy_connect_timeout 3s;
            proxy_read_timeout 10s;
            proxy_intercept_errors on;
            error_page 400 403 404 410 429 500 502 503 504 =204 /_maps_none;
            add_header Cache-Control \"max-age=86400\" always;
            add_header X-Map-Source \"online \$upstream_cache_status\" always;
        }"
    fi
  done
fi
export UILOC="$uiloc" ZONE="$zone" CONN="$conn" TLS="$tls" MEDILOC="$mediloc" MAPSLOC="$mapsloc" MAPSCACHE="$mapscache" MCHECK="$mcheck"
awk -v prefix="$prefix" -v sock="$sock" -v listen="$listen" -v mime="$mime" '
  BEGIN { uiloc = ENVIRON["UILOC"]; zone = ENVIRON["ZONE"]; conn = ENVIRON["CONN"]; tls = ENVIRON["TLS"]; mediloc = ENVIRON["MEDILOC"]
          mapsloc = ENVIRON["MAPSLOC"]; mapscache = ENVIRON["MAPSCACHE"]; mcheck = ENVIRON["MCHECK"] }
  { gsub(/@PREFIX@/, prefix); gsub(/@SOCK@/, sock); gsub(/@LISTEN@/, listen); gsub(/@MIME@/, mime)
    if ($0 == "@UI_LOCATION@") print uiloc
    else if ($0 == "@LIMIT_ZONE@") print zone
    else if ($0 == "@LIMIT_CONN@") print conn
    else if ($0 == "@TLS@") { if (tls != "") print tls }
    else if ($0 == "@MEDIA_LOCATION@") { if (mediloc != "") print mediloc }
    else if ($0 == "@MAPS_LOCATION@") { if (mapsloc != "") print mapsloc }
    else if ($0 == "@MAPS_CACHE@") { if (mapscache != "") print mapscache }
    else if ($0 == "@AUTH_CHECK@") { if (mcheck != "") print mcheck }
    else print }' "$here/nginx.conf.in" > "$out"
