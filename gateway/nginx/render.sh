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
# nginx에 ssl 모듈이 없으면 HTTPS를 빼고(경고), rewrite 모듈이 없으면 넘김만 뺀다.
set -euo pipefail
out="$1" prefix="$2" sock="$3" listen="$4" root="$5" ui="${6:-1}"
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
mkdir -p "$prefix/tmp"
mime=/etc/nginx/mime.types
if [[ ! -f "$mime" ]]; then
  mime="$prefix/mime.types"
  echo 'types { text/html html; text/css css; text/javascript js; application/json json; image/svg+xml svg; image/png png; }' > "$mime"
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
awk -v prefix="$prefix" -v sock="$sock" -v listen="$listen" -v mime="$mime" -v uiloc="$uiloc" -v zone="$zone" -v conn="$conn" -v tls="$tls" '
  { gsub(/@PREFIX@/, prefix); gsub(/@SOCK@/, sock); gsub(/@LISTEN@/, listen); gsub(/@MIME@/, mime)
    if ($0 == "@UI_LOCATION@") print uiloc
    else if ($0 == "@LIMIT_ZONE@") print zone
    else if ($0 == "@LIMIT_CONN@") print conn
    else if ($0 == "@TLS@") { if (tls != "") print tls }
    else print }' "$here/nginx.conf.in" > "$out"
