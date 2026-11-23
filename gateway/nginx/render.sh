#!/bin/bash
# nginx 설정 만들기(docs/gateway.md 2장): 틀 nginx.conf.in → 실행할 설정. 시작 셸과 시험(rest/test_nginx_front.py)이 같이 쓴다.
#
#   render.sh <출력 파일> <실행 폴더> <게이트웨이 소켓> <받는 주소:포트> <워크스페이스 폴더> [ui=1|0]
#
# 실행 폴더에 임시 폴더(tmp/)를 만든다. mime.types가 시스템에 없으면 최소 표를 만들어 쓴다.
# 깔린 nginx(NGINX_BIN 또는 PATH)에 limit_conn 모듈이 없으면(nginx -V에 --without-http_limit_conn_module,
# 예: Ubuntu 22.04 nginx-light) 주소당 연결 제한을 뺀다. 본문 크기·시간 제한은 그대로.
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
awk -v prefix="$prefix" -v sock="$sock" -v listen="$listen" -v mime="$mime" -v uiloc="$uiloc" -v zone="$zone" -v conn="$conn" '
  { gsub(/@PREFIX@/, prefix); gsub(/@SOCK@/, sock); gsub(/@LISTEN@/, listen); gsub(/@MIME@/, mime)
    if ($0 == "@UI_LOCATION@") print uiloc
    else if ($0 == "@LIMIT_ZONE@") print zone
    else if ($0 == "@LIMIT_CONN@") print conn
    else print }' "$here/nginx.conf.in" > "$out"
