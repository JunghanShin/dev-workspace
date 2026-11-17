#!/bin/bash
# PC 개발용 워크스페이스 실행. Jetson 없이 PC(WSL·Linux)에서 Manager + REST + logger와 워커를 띄운다.
# 보드용 workspace-start.sh와 같은 데이터 폴더 구조를 쓰지만, 다음은 하지 않는다:
#   - root 권한(일반 사용자로 실행), systemd 서비스
#   - 고정 IP 적용(network.conf), root tegrastats 수집기
#
#   dev/workspace-dev.sh                       이 터미널에서 실행. Ctrl+C로 종료
#                                              같은 데이터 폴더로 이미 실행 중이면 먼저 멈추고 다시 시작한다
#   dev/workspace-dev.sh --profile <이름|폴더>  처음 만들 때 프로필의 workspace.env·manager.json 사용
#
# 데이터 폴더: WORKSPACE_DATA(기본 ~/<워크스페이스 폴더 이름>_dev_data). 워커와 워커 설정은 워커 폴더(config.json·presets/, 보드와 같은 파일)
# (Jetson 전용 장치 워커는 PC용 실행 파일이 없으면 오류로 남는다. 시스템 모니터는 tegrastats가 없어 Manager가 사용 안 함으로 등록). 설정은 <데이터 폴더>/config/에서 고친다.
# 워커 실행 파일은 PC용으로 빌드해 둔다: workers/<워커>에서 jetson-build --native
set -uo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
DATA="${WORKSPACE_DATA:-$HOME/$(basename "$ROOT")_dev_data}"
DEFAULTS="$ROOT/install/config"
PROFILE=""
if [[ "${1:-}" == --profile ]]; then
  PROFILE="${2:?--profile needs a name or folder}"
  [[ "$PROFILE" != */* && -d "$ROOT/install/profiles/$PROFILE" ]] && PROFILE="$ROOT/install/profiles/$PROFILE"
  [[ -d "$PROFILE" ]] || { echo "profile folder not found: $PROFILE" >&2; exit 1; }
fi
PY="$(command -v python3 || true)"

log() { echo "workspace-dev: $*"; }

[[ -n "$PY" ]] || { log "python3 not found"; exit 1; }
[[ "$(id -u)" -ne 0 ]] || { log "run as a normal user (not root)"; exit 1; }

# ---- 0. 같은 데이터 폴더로 이미 실행 중이면 먼저 멈춘다(두 벌이 소켓·포트를 다투지 않게) ---------------
PIDFILE="$DATA/run/workspace-dev.pid"
old="$(cat "$PIDFILE" 2>/dev/null)"
# pid 번호가 다른 프로그램에 재사용됐을 수 있으므로 실제 이 스크립트인지 확인한다
if [[ -n "$old" && "$old" != "$$" ]] && kill -0 "$old" 2>/dev/null &&
   tr '\0' ' ' < "/proc/$old/cmdline" 2>/dev/null | grep -q "workspace-dev.sh"; then
  log "already running (pid $old): stopping first"
  kill -TERM "$old"
  for (( i = 0; i < 200; i++ )); do kill -0 "$old" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$old" 2>/dev/null; then log "previous instance (pid $old) did not stop in 20s"; exit 1; fi
fi

# ---- 1. 데이터 폴더 -----------------------------------------------------------------
mkdir -p "$DATA/config/worker-types.d" "$DATA/state" "$DATA/records" "$DATA/run" "$DATA/sdk"
# 더 이상 쓰지 않는 데이터 파일 정리(운용 모드 0.25에서 없앰, 예전 workers.json 옮긴 흔적): 지금 구성과 맞춘다
for f in config/mode config/workers.json.moved state/workers.json.bak; do
  if [[ -e "$DATA/$f" ]]; then rm -f "$DATA/$f"; echo "workspace-dev: removed obsolete $DATA/$f"; fi
done
echo $$ > "$PIDFILE"
seed() {  # seed <파일> <기본값 내용>: 없을 때만 만든다
  local dst="$DATA/config/$1"
  [[ -e "$dst" ]] && return 0
  if [[ -f "$DEFAULTS/$1" ]]; then cp "$DEFAULTS/$1" "$dst"; else printf '%s\n' "$2" > "$dst"; fi
  log "created $dst (default)"
}
# 프로필이 있으면 그 설정으로 처음 만든다(이미 있으면 그대로)
for f in workspace.env manager.json; do
  if [[ -n "$PROFILE" && -f "$PROFILE/$f" && ! -e "$DATA/config/$f" ]]; then
    cp "$PROFILE/$f" "$DATA/config/$f"
    log "created $DATA/config/$f (profile)"
  fi
done
seed workspace.env ""
seed manager.json "{}"
seed worker-types.json "{}"

# ---- 2. 워커 준비 (type 등록) ------------------------------------------------------------
rm -f "$DATA"/config/worker-types.d/*.json
: > "$DATA/run/payload.env"
for wdir in "$ROOT"/workers/*/; do
  [[ "$(basename "$wdir")" == _* ]] && continue  # 틀(_template)
  prep="${wdir}workspace/prepare.sh"            # 워크스페이스 연동 파일(예전 워커는 폴더 맨 위)
  [[ -f "$prep" ]] || prep="${wdir}prepare.sh"
  [[ -f "$prep" ]] || continue
  WORKSPACE_ROOT="$ROOT" WORKSPACE_DATA="$DATA" bash "$prep" 2>&1 | sed 's/^/  /'
done

# ---- 3. 실행 ------------------------------------------------------------------------------
export WORKSPACE_DATA="$DATA" PYTHONDONTWRITEBYTECODE=1
unset WORKSPACE_MANAGER_SETTINGS WORKSPACE_MANAGER_SOCKET WORKSPACE_LOGGER_DIR
set -a; [[ -f "$DATA/config/workspace.env" ]] && . "$DATA/config/workspace.env"; set +a

PIDS=()
start() {  # start <PYTHONPATH> <모듈>
  PYTHONPATH="$1" "$PY" -m "$2" &
  PIDS+=("$!")
}
stop_all() {
  local i
  for (( i = ${#PIDS[@]} - 1; i >= 0; i-- )); do kill -TERM "${PIDS[$i]}" 2>/dev/null; done
  wait 2>/dev/null
  [[ "$(cat "$PIDFILE" 2>/dev/null)" == "$$" ]] && rm -f "$PIDFILE"
  log "stopped"
}
trap 'stop_all; exit 0' INT TERM

rm -f "$DATA/run/manager.sock"
start "$ROOT/manager" workspace_manager
for (( i = 0; i < 100; i++ )); do [[ -S "$DATA/run/manager.sock" ]] && break; sleep 0.1; done
[[ -S "$DATA/run/manager.sock" ]] || { log "manager did not start"; stop_all; exit 1; }
start "$ROOT/rest" workspace_rest
start "$ROOT/logger" workspace_logger
log "workspace=$ROOT data=$DATA REST=http://localhost:${WORKSPACE_REST_PORT:-8080}  (Ctrl+C to stop)"
wait -n
log "a component exited; stopping all"
stop_all
exit 1
