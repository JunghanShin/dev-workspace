#!/bin/bash
# nginx 설정 적용(docs/live-settings.md): workspace.env → 인증서 → render.sh → nginx -t → (다시 읽기).
# 시작 셸(workspace-start.sh)과 게이트웨이(설정 탭 바로 적용)가 같은 이 스크립트를 쓴다: 시작과 바로 적용이 늘 같은 설정을 만든다.
# 워크스페이스 계정으로 돈다(nginx·게이트웨이와 같은 계정, 실행 폴더·<데이터>/config/tls/도 그 계정 것).
#
#   apply.sh <데이터> <워크스페이스> <게이트웨이 소켓> [--reload]
#
#   --reload 없이  설정만 만들고 점검(시작 셸: 그 뒤 nginx를 띄움)
#   --reload       점검에 통과하면 떠 있는 nginx에 다시 읽기(nginx -s reload: 연결을 끊지 않고 새 worker로 바꿈).
#                  옛 worker가 새 연결을 그만 받을 때까지 기다린 뒤 돌아옴(최대 5 s)
#
# 읽는 키(<데이터>/config/workspace.env, 파일에 없으면 기본값. 환경 변수의 같은 키는 쓰지 않는다: 지운 칸이 시작 값으로 남지 않게)
#   WORKSPACE_REST_BIND·_PORT, WORKSPACE_UI, WORKSPACE_HTTPS·_PORT·_CERT·_KEY, WORKSPACE_HTTP_REDIRECT, WORKSPACE_AUTH
#   로그인: WORKSPACE_AUTH=on 또는 제품(환경 WORKSPACE_PRODUCT=1)이면 RENDER_AUTH=on(/media/도 로그인, docs/media-auth.md)
# 실행 폴더: WORKSPACE_NGINX_DIR(기본 <데이터>/run/nginx). nginx: NGINX_BIN 또는 PATH. RENDER_MEDIA·CERT_*는 그대로 넘긴다.
#
# 결과: 표준 출력 마지막 줄 "nginx: applied ..." / 실패면 표준 오류 마지막 줄에 이유, 종료 코드 1(설정 없음·점검 실패, 옛 설정 그대로),
#       2(인자), 3(다시 읽기 실패)
set -uo pipefail
if (( $# < 3 )); then
  echo "usage: apply.sh <data> <workspace> <gateway socket> [--reload]" >&2
  exit 2
fi
data="$1" root="$2" sock="$3" reload="${4:-}"
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
dir="${WORKSPACE_NGINX_DIR:-$data/run/nginx}"
nginx_bin="${NGINX_BIN:-$(command -v nginx 2>/dev/null || echo /usr/sbin/nginx)}"
export NGINX_BIN="$nginx_bin"
fail() { echo "nginx: $*" >&2; exit "${code:-1}"; }

# workspace.env 읽기(실행하지 않음, workspace-start.sh read_kv와 같은 규칙: # 뒤 버림, 끝 공백·앞뒤 " 뗌, 같은 키는 뒤의 것)
KEYS=(WORKSPACE_REST_BIND WORKSPACE_REST_PORT WORKSPACE_UI WORKSPACE_HTTPS WORKSPACE_HTTPS_PORT WORKSPACE_HTTPS_CERT
      WORKSPACE_HTTPS_KEY WORKSPACE_HTTP_REDIRECT WORKSPACE_AUTH)
for k in "${KEYS[@]}"; do unset "$k"; done
envf="$data/config/workspace.env"
if [[ -f "$envf" ]]; then
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%%#*}"
    [[ "$line" =~ ^[[:space:]]*([A-Z_][A-Z0-9_]*)=(.*)$ ]] || continue
    key="${BASH_REMATCH[1]}" value="${BASH_REMATCH[2]}"
    value="${value%"${value##*[![:space:]]}"}"
    value="${value#\"}"; value="${value%\"}"
    for k in "${KEYS[@]}"; do [[ "$k" == "$key" ]] && printf -v "$key" '%s' "$value"; done
  done < "$envf"
fi
bind="${WORKSPACE_REST_BIND:-0.0.0.0}"
port="${WORKSPACE_REST_PORT:-8080}"

# HTTPS(docs/https.md): 사용자 인증서 또는 보드 자체 서명(<데이터>/config/tls/, 없거나 만료 30일 안·주소가 바뀌면 다시 만듦)
tls_env=()
https=""
if [[ "${WORKSPACE_HTTPS:-off}" == on ]]; then
  crt="${WORKSPACE_HTTPS_CERT:-}" key="${WORKSPACE_HTTPS_KEY:-}"
  if [[ -z "$crt" ]]; then
    crt="$data/config/tls/workspace.crt" key="$data/config/tls/workspace.key"
    bash "$here/make-cert.sh" "$data/config/tls" | sed 's/^/tls: /' || fail "https: cannot make the board certificate"
  elif ! openssl x509 -checkend 2592000 -noout -in "$crt" >/dev/null 2>&1; then
    echo "https: certificate $crt expires within 30 days or cannot be read" >&2
  fi
  if [[ -r "$crt" && -r "$key" ]]; then
    tls_env=(RENDER_TLS_LISTEN="$bind:${WORKSPACE_HTTPS_PORT:-8443}" RENDER_TLS_CERT="$crt" RENDER_TLS_KEY="$key"
             RENDER_REDIRECT="${WORKSPACE_HTTP_REDIRECT:-on}")
    https=", https ${WORKSPACE_HTTPS_PORT:-8443}"
  else
    # 시작 셸: 평문만으로 계속(예전과 같음). 바로 적용: 옛 설정을 두고 실패로 알림
    [[ "$reload" == --reload ]] && fail "https: certificate or key not readable by the workspace account ($crt, $key)"
    echo "https: certificate or key not readable by the workspace account; plain HTTP only" >&2
  fi
fi
auth=off
[[ "${WORKSPACE_AUTH:-off}" == on || "${WORKSPACE_PRODUCT:-0}" == 1 ]] && auth=on

mkdir -p "$dir" || fail "cannot make $dir"
new="$dir/nginx.conf.new"
env ${tls_env[@]+"${tls_env[@]}"} RENDER_AUTH="$auth" bash "$here/render.sh" "$new" "$dir" "$sock" "$bind:$port" "$root" \
  "${WORKSPACE_UI:-1}" || fail "render.sh failed"
if ! out="$("$nginx_bin" -t -q -p "$dir" -c "$new" -g 'daemon off;' 2>&1)"; then
  rm -f "$new"
  [[ -n "$out" ]] && echo "$out" >&2
  fail "config test failed (kept the previous config): $(tail -n 1 <<<"$out")"
fi
mv -f "$new" "$dir/nginx.conf"
if [[ "$reload" == --reload ]]; then
  # nginx -s reload는 신호만 보내고 돌아온다. 옛 worker가 QUIT를 받아 새 연결을 그만 받기 전에는 옛 설정으로도 답하므로
  # "적용됨"은 옛 worker가 모두 끝났거나 "shutting down"이 된 뒤에(열린 스트림은 옛 worker가 끝까지 들고 있어도 됨)
  master="$(cat "$dir/nginx.pid" 2>/dev/null || true)"
  old_workers=""
  [[ -n "$master" ]] && old_workers="$(pgrep -P "$master" 2>/dev/null | tr '\n' ' ' || true)"
  if ! out="$("$nginx_bin" -p "$dir" -c "$dir/nginx.conf" -g 'daemon off;' -s reload 2>&1)"; then
    [[ -n "$out" ]] && echo "$out" >&2
    code=3 fail "reload failed: $(tail -n 1 <<<"$out")"
  fi
  switched=0
  for _ in $(seq 1 100); do  # 최대 5 s
    busy=0
    for w in $old_workers; do
      [[ -r /proc/$w/cmdline ]] || continue
      tr '\0' ' ' </proc/$w/cmdline | grep -q "shutting down" || busy=1
    done
    (( busy )) || { switched=1; break; }
    sleep 0.05
  done
  (( switched )) || echo "apply.sh: nginx reloaded but old worker still taking connections after 5 s" >&2
  echo "nginx: applied and reloaded (listen $bind:$port$https, login $auth)"
else
  echo "nginx: config ready (listen $bind:$port$https, login $auth)"
fi
