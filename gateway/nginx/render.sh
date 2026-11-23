#!/bin/bash
# nginx 설정 만들기(docs/gateway.md 2장): 틀 nginx.conf.in → 실행할 설정. 시작 셸과 시험(rest/test_nginx_front.py)이 같이 쓴다.
#
#   render.sh <출력 파일> <실행 폴더> <게이트웨이 소켓> <받는 주소:포트> <워크스페이스 폴더> [ui=1|0]
#
# 실행 폴더에 임시 폴더(tmp/)를 만든다. mime.types가 시스템에 없으면 최소 표를 만들어 쓴다.
set -euo pipefail
out="$1" prefix="$2" sock="$3" listen="$4" root="$5" ui="${6:-1}"
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
mkdir -p "$prefix/tmp"
mime=/etc/nginx/mime.types
if [[ ! -f "$mime" ]]; then
  mime="$prefix/mime.types"
  echo 'types { text/html html; text/css css; text/javascript js; application/json json; image/svg+xml svg; image/png png; }' > "$mime"
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
awk -v prefix="$prefix" -v sock="$sock" -v listen="$listen" -v mime="$mime" -v uiloc="$uiloc" \
  '{ gsub(/@PREFIX@/, prefix); gsub(/@SOCK@/, sock); gsub(/@LISTEN@/, listen); gsub(/@MIME@/, mime);
     if ($0 == "@UI_LOCATION@") print uiloc; else print }' \
  "$here/nginx.conf.in" > "$out"
