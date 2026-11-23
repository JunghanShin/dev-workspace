#!/bin/bash
# 워커 여러 개를 워크스페이스 없이 함께 띄우고 함께 내린다 (design.md 6.6 독립 실행).
# 워크스페이스에서 Manager가 하는 일(실행, 끝나면 다시 실행, 정지 시 정리)을 단순하게 대신한다. heartbeat 감시는 없다.
#
#   run-workers.sh <실행 파일> <설정 파일> [<실행 파일> <설정 파일> ...]
#   예) run-workers.sh workers/cam/build/cam-worker cam.json \
#                      workers/video/build/video-worker video-0.json \
#                      workers/video/build/video-worker video-0-proc.json
#
# - 같은 설정 파일로 이미 돌고 있으면(다른 run-workers.sh, 또는 워커 프로세스) 먼저 멈추고 띄운다
# - 워커가 끝나면 RESTART_SEC(기본 2초) 뒤 다시 띄운다. 설정 오류(exit 1)면 다시 띄우지 않는다
# - Ctrl+C 또는 SIGTERM: 모든 워커에 SIGTERM, STOP_SEC(기본 6초) 안에 안 끝나면 SIGKILL
# - 로그는 화면에 "[설정 이름] ..." 형식으로 섞어 낸다. 명령 소켓은 각 워커의 규칙대로
#   (command_socket 설정 → $WORKSPACE_DATA → $XDG_RUNTIME_DIR → /tmp)
set -uo pipefail

RESTART_SEC="${RESTART_SEC:-2}"
STOP_SEC="${STOP_SEC:-6}"

if (( $# == 0 || $# % 2 != 0 )); then
    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

# ---- 같은 설정으로 이미 돌고 있는 것은 먼저 멈춘다 -------------------------------------------
# 같은 설정 파일을 쓰는 run-workers.sh(그 워커들을 다시 띄우므로 먼저)와, 같은 설정 파일로 떠 있는 워커 프로세스.
# 설정 파일은 실제 경로로 비교한다. 두 벌이 같은 명령 소켓·카메라를 다투지 않게 한다
cfgs=()
for (( i = 2; i <= $#; i += 2 )); do cfgs+=("$(readlink -f "${!i}" 2>/dev/null || echo "${!i}")"); done

uses_cfg() {  # uses_cfg <pid> <모드: runner|worker> : 그 프로세스가 우리 설정 파일 중 하나를 쓰면 0
    local pid="$1" mode="$2" a prev="" c
    [[ -r "/proc/$pid/cmdline" ]] || return 1
    while IFS= read -r -d '' a; do
        if [[ "$mode" == worker && "$prev" != "--config" ]]; then prev="$a"; continue; fi
        a="$(cd "/proc/$pid/cwd" 2>/dev/null && readlink -f "$a" 2>/dev/null || echo "$a")"
        for c in "${cfgs[@]}"; do [[ "$a" == "$c" ]] && return 0; done
        prev="$a"
    done < "/proc/$pid/cmdline"
    return 1
}

stop_pids() {  # stop_pids <이름> <pid...> : TERM, STOP_SEC 안에 안 끝나면 KILL
    local what="$1" p end; shift
    (( $# )) || return 0
    echo "already running ($what, pid $*): stopping first"
    kill -TERM "$@" 2>/dev/null
    end=$((SECONDS + STOP_SEC))
    while (( SECONDS < end )); do
        local alive=0
        for p in "$@"; do kill -0 "$p" 2>/dev/null && alive=1; done
        (( alive )) || return 0
        sleep 0.2
    done
    kill -KILL "$@" 2>/dev/null
}

old=()
for p in $(pgrep -f "run-workers\.sh" 2>/dev/null); do
    [[ "$p" == "$$" || "$p" == "$PPID" ]] && continue
    [[ "$(ps -o ppid= -p "$p" 2>/dev/null | tr -d ' ')" == "$$" ]] && continue  # 이 스크립트의 서브셸
    uses_cfg "$p" runner && old+=("$p")
done
stop_pids "run-workers.sh" "${old[@]}"
old=()
for p in $(pgrep -f -- "--config" 2>/dev/null); do
    [[ "$p" == "$$" ]] && continue
    uses_cfg "$p" worker && old+=("$p")
done
stop_pids "worker" "${old[@]}"

pids=()
names=()
stopping=0

supervise() {  # 한 워커를 계속 띄운다(서브셸)
    local exe="$1" cfg="$2" name
    name="$(basename "${cfg%.*}")"
    local child=0
    trap 'stopping=1; [[ $child -gt 0 ]] && kill -TERM "$child" 2>/dev/null' TERM
    while (( !stopping )); do
        "$exe" --config "$cfg" > >(sed -u "s/^/[$name] /") 2>&1 &
        child=$!
        wait "$child"
        local rc=$?
        # trap으로 wait가 끊기면 자식이 끝날 때까지 기다린다
        while kill -0 "$child" 2>/dev/null; do wait "$child"; rc=$?; done
        child=0
        (( stopping )) && break
        if (( rc == 1 )); then
            echo "[$name] exited 1 (config error); not restarting"
            break
        fi
        echo "[$name] exited $rc; restarting in ${RESTART_SEC}s"
        sleep "$RESTART_SEC" & wait $!
    done
}

while (( $# )); do
    [[ -x "$1" ]] || { echo "not executable: $1" >&2; exit 2; }
    [[ -f "$2" ]] || { echo "no config file: $2" >&2; exit 2; }
    supervise "$1" "$2" &
    pids+=($!)
    names+=("$(basename "${2%.*}")")
    shift 2
done
echo "running: ${names[*]} (Ctrl+C to stop)"

stop_all() {
    trap '' INT TERM
    echo "stopping: ${names[*]}"
    kill -TERM "${pids[@]}" 2>/dev/null
    local end=$((SECONDS + STOP_SEC))
    while (( SECONDS < end )); do
        local alive=0
        for p in "${pids[@]}"; do kill -0 "$p" 2>/dev/null && alive=1; done
        (( alive )) || exit 0
        sleep 0.2
    done
    echo "still running after ${STOP_SEC}s; killing"
    pkill -KILL -g $$ 2>/dev/null
    exit 1
}
trap stop_all INT TERM
wait
