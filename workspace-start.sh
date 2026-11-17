#!/bin/bash
# 워크스페이스 시작 (design.md 11.2).
#
#   ./workspace-start.sh               systemd 서비스(workspace.service)로 시작. root가 아니면 sudo로 다시 실행
#                                      이미 실행 중이면(서비스·--foreground 모두) 먼저 멈추고 다시 시작한다
#   ./workspace-start.sh --foreground  서비스 없이 이 터미널에서 실행(디버깅). Ctrl+C로 종료
#   workspace-start.sh --service       systemd 진입점(workspace.service의 ExecStart). 직접 쓰지 않는다
#
# 하는 일 (--service / --foreground):
#   1. 데이터 폴더(<워크스페이스>_data) 확인. 없거나 지워진 폴더·파일은 기본값으로 다시 만든다
#   2. 보드 상태 수집(root tegrastats), 네트워크 적용(network.conf), 워커 준비(workers/*/workspace/prepare.sh, 예전 워커는 workers/*/prepare.sh).
#      실패해도 경고만 하고 계속
#   3. Manager → REST → logger 를 워크스페이스 소유자 계정으로 실행하고 감시
#   4. 종료 요청(SIGTERM, workspace-stop.sh) 시 REST → Manager(워커 정리) → logger 순서로 종료
#
# 자동 재실행은 없다. Manager가 죽으면 전체를 내리고 끝난다. REST·logger가 죽으면 기록만 하고 계속.
set -uo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
DATA="${WORKSPACE_DATA:-${ROOT}_data}"
DEFAULTS="$ROOT/install/config"
PY=/usr/bin/python3
# 제품 설치(dev/build-product.sh의 install.sh)는 서비스 이름·실행 계정을 서비스 환경으로 넘긴다
UNIT="${WORKSPACE_UNIT:-workspace.service}"

log() { echo "workspace-start: $*"; }

# 이미 실행 중인 워크스페이스는 항상 먼저 멈춘다(두 벌이 뜨거나, 예전 프로그램이 그대로 남지 않게).
# 서비스로 떠 있는 것과 서비스 없이(--foreground) 떠 있는 것 모두. 정리 순서는 workspace-stop.sh와 같다
stop_previous() {
  local keep_service="${1:-}" pid i
  if [[ -z "$keep_service" ]] && systemctl is-active --quiet "$UNIT" 2>/dev/null; then
    log "already running ($UNIT): stopping first"
    systemctl stop "$UNIT"
  fi
  pid="$(cat "$DATA/run/workspace-start.pid" 2>/dev/null)"
  # pid 파일이 남아 있어도 그 번호가 다른 프로그램에 재사용됐을 수 있으므로 실제 시작 셸인지 확인한다
  if [[ -n "$pid" && "$pid" != "$$" ]] && kill -0 "$pid" 2>/dev/null &&
     tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q "workspace-start.sh"; then
    log "already running (pid $pid): stopping first"
    kill -TERM "$pid"
    for (( i = 0; i < 400; i++ )); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
      log "previous instance (pid $pid) did not stop in 40s"
      exit 1
    fi
  fi
}

# ---- 서비스 시작(인자 없음) -----------------------------------------------------
if (( $# == 0 )); then
  [[ "$(id -u)" -eq 0 ]] || exec sudo bash "$0"
  if [[ ! -f "/etc/systemd/system/$UNIT" ]]; then
    log "$UNIT is not installed. Run: sudo bash $ROOT/install/install_workspace.sh"
    log "or run without the service: sudo bash $0 --foreground"
    exit 1
  fi
  stop_previous
  systemctl start "$UNIT"
  systemctl --no-pager status "$UNIT" --lines=15 || true
  exit 0
fi

case "$1" in
  --service | --foreground) ;;
  *) echo "usage: $0 [--foreground]" >&2; exit 2 ;;
esac
if [[ "$(id -u)" -ne 0 ]]; then
  [[ "$1" == --foreground ]] && exec sudo bash "$0" --foreground
  log "must run as root"; exit 1
fi
# --service는 systemd가 이 서비스를 띄우는 중이므로 서비스는 건드리지 않고, 서비스 없이 떠 있는 것만 멈춘다
if [[ "$1" == --service ]]; then stop_previous keep_service; else stop_previous; fi

# 프로그램은 워크스페이스 소유자 계정으로 실행한다(root로 돌리지 않음)
OWNER="${WORKSPACE_OWNER:-$(stat -c %U "$ROOT")}"
OWNER_UID="$(id -u "$OWNER")"
OWNER_GID="$(id -g "$OWNER")"
if [[ "$OWNER_UID" -eq 0 ]]; then
  log "warning: workspace is owned by root; programs will run as root"
fi
# 제품 묶음은 Manager·REST·logger가 컴파일된 실행 파일(app/)이라 python3가 필요 없다
[[ -d "$ROOT/app" || -x "$PY" ]] || { log "python3 not found: $PY"; exit 1; }

# ---- 1. 데이터 폴더 -----------------------------------------------------------------
seed() {  # seed <파일> <기본값 파일 또는 내용>  : 없을 때만 만든다
  local dst="$DATA/config/$1"
  [[ -e "$dst" ]] && return 0
  if [[ -f "$DEFAULTS/$1" ]]; then
    cp "$DEFAULTS/$1" "$dst"
  else
    printf '%s\n' "$2" > "$dst"
  fi
  log "created $dst (default)"
}

mkdir -p "$DATA/config/worker-types.d" "$DATA/state" "$DATA/records" "$DATA/run" "$DATA/sdk"
# 더 이상 쓰지 않는 데이터 파일 정리(운용 모드 0.25에서 없앰, 예전 workers.json 옮긴 흔적): 지금 구성과 맞춘다
for f in config/mode config/workers.json.moved state/workers.json.bak; do
  if [[ -e "$DATA/$f" ]]; then rm -f "$DATA/$f"; log "removed obsolete $DATA/$f"; fi
done
seed network.conf "ENABLED=no"
seed workspace.env ""
seed manager.json "{}"
seed worker-types.json "{}"
# 로컬 미디어 서버 설정(workspace-mediamtx.service가 읽음). 원격 중계 주소가 현장 값
seed mediamtx.yml ""
# 페이로드 type 조각은 매번 다시 만든다(지금 워크스페이스에 있는 페이로드만)
rm -f "$DATA"/config/worker-types.d/*.json
: > "$DATA/run/payload.env"

# KEY=VALUE 파일 읽기(실행하지 않는다). read_kv <파일> <허용 키...>
read_kv() {
  local file="$1"; shift
  local line key value allowed
  [[ -f "$file" ]] || return 0
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%%#*}"
    [[ "$line" =~ ^[[:space:]]*([A-Z_][A-Z0-9_]*)=(.*)$ ]] || continue
    key="${BASH_REMATCH[1]}"
    value="${BASH_REMATCH[2]}"
    value="${value%"${value##*[![:space:]]}"}"
    value="${value#\"}"; value="${value%\"}"
    allowed=0
    for k in "$@"; do [[ "$k" == "$key" || "$k" == "*" ]] && allowed=1; done
    if (( allowed )); then
      export "$key=$value"
    else
      log "ignored $key in $file (paths are set by workspace-start.sh)"
    fi
  done < "$file"
}

# 실행 설정. 경로 관련 키는 받지 않는다(이 스크립트가 데이터 폴더 기준으로 정한다)
read_kv "$DATA/config/workspace.env" WORKSPACE_REST_BIND WORKSPACE_REST_PORT WORKSPACE_REST_COMMAND_TIMEOUT WORKSPACE_UI_MEDIA_URL WORKSPACE_UI_TITLE WORKSPACE_LOG_LEVEL \
  WORKSPACE_REST_TOKEN WORKSPACE_UI \
  WORKSPACE_LOGGER_INTERVAL_S WORKSPACE_LOGGER_MAX_FILE_MB WORKSPACE_LOGGER_MAX_TOTAL_MB

# ---- 2. 보드 상태 수집, 네트워크, 워커 준비 (root) -----------------------------------
# tegrastats는 root로 돌려야 EMC·NVENC·VIC·DLA 항목이 나온다. 최신 한 줄을 sysmon이 읽는다.
# 워커가 뜨기 전에 첫 줄이 생기도록 먼저 띄운다. 없거나 죽으면 sysmon이 직접 실행(일부 항목 없음)
COLLECT=""
rm -f "$DATA/run/tegrastats.line"
if command -v tegrastats >/dev/null; then
  bash "$ROOT/install/bin/workspace-tegrastats-collect" "$DATA/run/tegrastats.line" 1000 &
  COLLECT=$!
  log "tegrastats collector started as root (pid $COLLECT)"
fi

if ! bash "$ROOT/install/bin/workspace-network-apply" "$DATA/config/network.conf"; then
  log "warning: network apply failed; continuing with current network"
fi
# 모든 워커 폴더의 type을 등록한다(워커 = 워커 폴더, 설정은 그 폴더의 config.json·presets/).
# 그래야 나중에 REST로 새 워커를 등록할 때 type을 찾는다
# 워커의 워크스페이스 연동 파일은 workers/<워커>/workspace/(예전 워커는 폴더 맨 위). _로 시작하는 폴더(틀)는 뺀다
for wdir in "$ROOT"/workers/*/; do
  name="$(basename "$wdir")"
  [[ "$name" == _* ]] && continue
  prep="${wdir}workspace/prepare.sh"
  [[ -f "$prep" ]] || prep="${wdir}prepare.sh"
  [[ -f "$prep" ]] || continue
  if ! WORKSPACE_ROOT="$ROOT" WORKSPACE_DATA="$DATA" bash "$prep"; then
    log "warning: worker $name prepare failed; its workers may not start"
  fi
done
chown -R "$OWNER_UID:$OWNER_GID" "$DATA"

# 워커 준비가 넘길 경로(PYTHONPATH, LD_LIBRARY_PATH). 같은 키는 ':'로 이어 붙인다
EXTRA_PYTHONPATH=""
EXTRA_LD=""
while IFS='=' read -r key value; do
  case "$key" in
    PYTHONPATH) EXTRA_PYTHONPATH="${EXTRA_PYTHONPATH:+$EXTRA_PYTHONPATH:}$value" ;;
    LD_LIBRARY_PATH) EXTRA_LD="${EXTRA_LD:+$EXTRA_LD:}$value" ;;
  esac
done < "$DATA/run/payload.env"

# ---- 3. 실행·감시 ---------------------------------------------------------------------
export WORKSPACE_DATA="$DATA" WORKSPACE_ROOT="$ROOT" PYTHONDONTWRITEBYTECODE=1
unset WORKSPACE_MANAGER_SETTINGS WORKSPACE_MANAGER_SOCKET WORKSPACE_LOGGER_DIR

OWNER_HOME="$(getent passwd "$OWNER" | cut -d: -f6)"
run_as_owner() {  # run_as_owner <PYTHONPATH> <모듈> [환경 KEY=VALUE...]
  local pypath="$1" module="$2" cmd; shift 2
  # 제품 묶음: app/<모듈>/<모듈>(Nuitka로 컴파일한 실행 파일)이 있으면 그것을, 없으면 python3 -m <모듈>
  if [[ -x "$ROOT/app/$module/$module" ]]; then cmd=("$ROOT/app/$module/$module"); else cmd=("$PY" -m "$module"); fi
  env "$@" PYTHONPATH="$pypath" HOME="$OWNER_HOME" USER="$OWNER" LOGNAME="$OWNER" \
    setpriv --reuid="$OWNER_UID" --regid="$OWNER_GID" --init-groups \
    "${cmd[@]}" &
}

alive() { [[ -n "${1:-}" ]] && kill -0 "$1" 2>/dev/null; }

stop_pid() {  # stop_pid <pid> <이름> <timeout s>
  local pid="$1" name="$2" timeout="$3" i
  alive "$pid" || return 0
  log "stopping $name (pid $pid)"
  kill -TERM "$pid" 2>/dev/null
  for (( i = 0; i < timeout * 10; i++ )); do
    alive "$pid" || { wait "$pid" 2>/dev/null; return 0; }
    sleep 0.1
  done
  log "$name did not stop in ${timeout}s, SIGKILL"
  kill -KILL "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
}

MGR="${MGR:-}" REST="${REST:-}" LOGGER="${LOGGER:-}"
shutdown() {
  stop_pid "$REST" rest 5
  stop_pid "$MGR" manager 20      # Manager가 워커를 정리한다(워커 stop_timeout 포함)
  stop_pid "$LOGGER" logger 5     # Manager 종료까지 기록
  stop_pid "$COLLECT" tegrastats-collector 3
}

STOPPING=0
trap 'STOPPING=1' TERM INT
echo $$ > "$DATA/run/workspace-start.pid"

log "workspace=$ROOT data=$DATA user=$OWNER"

rm -f "$DATA/run/manager.sock"
run_as_owner "$ROOT/manager${EXTRA_PYTHONPATH:+:$EXTRA_PYTHONPATH}" workspace_manager \
  ${EXTRA_LD:+LD_LIBRARY_PATH="$EXTRA_LD"}
MGR=$!
for (( i = 0; i < 100; i++ )); do
  [[ -S "$DATA/run/manager.sock" ]] && break
  alive "$MGR" || break
  sleep 0.1
done
if [[ ! -S "$DATA/run/manager.sock" ]]; then
  log "manager did not start"
  shutdown
  exit 1
fi
log "manager started (pid $MGR)"

# REST(화면): 늘 띄운다(워커 개발 작업대, design.md 6.1)
run_as_owner "$ROOT/rest" workspace_rest
REST=$!
log "rest started (pid $REST)"
run_as_owner "$ROOT/logger" workspace_logger
LOGGER=$!
log "logger started (pid $LOGGER)"

code=0
while (( ! STOPPING )); do
  wait -n 2>/dev/null
  (( STOPPING )) && break
  if ! alive "$MGR"; then
    log "manager exited; stopping all (no automatic restart)"
    code=1
    break
  fi
  if [[ -n "$REST" ]] && ! alive "$REST"; then
    log "rest exited; manager keeps running (start again with workspace-stop.sh / workspace-start.sh)"
    REST=""
  fi
  if [[ -n "$LOGGER" ]] && ! alive "$LOGGER"; then
    log "logger exited; manager keeps running (no records until restart)"
    LOGGER=""
  fi
  if [[ -n "$COLLECT" ]] && ! alive "$COLLECT"; then
    log "tegrastats collector exited; sysmon falls back to its own tegrastats (no EMC/HW engine values)"
    COLLECT=""
  fi
done

shutdown
rm -f "$DATA/run/workspace-start.pid"
log "stopped"
exit "$code"
