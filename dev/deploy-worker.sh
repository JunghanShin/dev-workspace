#!/bin/bash
# 워커 하나만 빠르게 보드에 반영한다(개발용). 워크스페이스 전체 재시작·sudo 없이 그 워커만 다시 시작한다.
#
#   dev/deploy-worker.sh <워커>              Jetson 빌드 → 보드 workers/<워커>/build/에 복사 → 그 워커만 다시 시작
#   dev/deploy-worker.sh --test <워커>       빌드할 때 시험도(jetson-build --test)
#   dev/deploy-worker.sh --no-build <워커>   빌드 없이 지난 빌드(build/jetson/install/bin)를 복사
#   dev/deploy-worker.sh --revert <워커>     보드의 개발 빌드를 지우고 다시 시작 → 확정 실행 파일(workspace/bin/)로 돌아감
#
# 보드 접속(저장소에 넣지 않는다): 환경 변수 또는 dev/board.env(깃 제외, 예: dev/board.env.example)
#   WORKSPACE_BOARD=user@host  WORKSPACE_BOARD_PORT=22  WORKSPACE_BOARD_DIR=~/dev-workspace  WORKSPACE_BOARD_REST=8080
#
# 동작 원리(design.md 11.3): prepare.sh가 type에 실행 파일 후보(build/ → workspace/bin/)를 적고, Manager는 워커를 띄울 때마다
# 있는 첫째를 쓴다. 그래서 build/에 올리고 그 워커만 다시 시작하면 새 실행 파일로 뜬다. 보드 깃 작업 트리는 건드리지 않는다
# (build/는 깃 제외). 확정은 지금처럼: 커밋 → dev/release-worker.sh → workspace/bin 커밋 → 보드 git pull
set -euo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
[[ -f "$ROOT/dev/board.env" ]] && . "$ROOT/dev/board.env"
BUILD=1 TEST=0 REVERT=0
while [[ "${1:-}" == --* ]]; do
  case "$1" in
    --no-build) BUILD=0 ;;
    --test) TEST=1 ;;
    --revert) REVERT=1 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
NAME="${1:-}"
W="$ROOT/workers/$NAME"
if [[ -z "$NAME" || ! -d "$W/workspace" ]]; then
  echo "usage: dev/deploy-worker.sh [--test|--no-build|--revert] <worker folder>" >&2
  exit 2
fi
: "${WORKSPACE_BOARD:?set WORKSPACE_BOARD=user@host (env or dev/board.env)}"
PORT="${WORKSPACE_BOARD_PORT:-22}"
RDIR="${WORKSPACE_BOARD_DIR:-}"
[[ -n "$RDIR" ]] || RDIR='~/dev-workspace'
# board.env의 ~는 이 PC에서 먼저 풀린다 → 보드에서 풀리도록 되돌린다(보드 홈 기준)
[[ "$RDIR" == "$HOME"/* ]] && RDIR="~/${RDIR#"$HOME"/}"
REST="${WORKSPACE_BOARD_REST:-8080}"
SSH=(ssh -p "$PORT" -o BatchMode=yes -o ConnectTimeout=10 "$WORKSPACE_BOARD")

# 보드 쪽: 그 워커만 다시 시작하고 결과를 보여 준다(REST는 보드 안 127.0.0.1로)
restart_remote() {
  "${SSH[@]}" "python3 - '$NAME' '$REST'" <<'PY'
import json, sys, time, urllib.request
wid, port = sys.argv[1], sys.argv[2]
base = "http://127.0.0.1:%s" % port
def req(method, path):
    r = urllib.request.Request(base + path, method=method, data=b"{}" if method == "POST" else None,
                               headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=5) as f:
        return json.loads(f.read().decode() or "null")
def state():
    return req("GET", "/workers/" + wid)
w = state()
if w["desired_state"] != "running":
    print("%s: 정지 상태라 다시 시작하지 않음(다음에 시작하면 새 실행 파일로 뜸)" % wid)
    sys.exit(0)
req("POST", "/workers/%s/stop" % wid)
end = time.time() + 15
while time.time() < end and state()["actual_state"] not in ("stopped", "exited"):
    time.sleep(0.2)
req("POST", "/workers/%s/start" % wid)
end = time.time() + 20
while time.time() < end:
    w = state()
    if w["actual_state"] == "running" and w.get("last_heartbeat_ms_ago") is not None:
        break
    time.sleep(0.3)
dev = "/build/" in (w.get("exec") or "")
print("%s: %s, libworker %s, 실행 파일 %s (%s)" % (wid, w["actual_state"], w.get("libworker"), w.get("exec"),
                                                "개발 빌드" if dev else "확정"))
sys.exit(0 if w["actual_state"] == "running" else 1)
PY
}

ABS="$("${SSH[@]}" "cd $RDIR/workers/$NAME && pwd")"  # 보드의 워커 폴더(절대 경로)

if (( REVERT )); then
  # build/ 바로 아래의 실행 파일(이 스크립트가 올린 것)만 지운다. 하위 폴더는 건드리지 않음
  "${SSH[@]}" "cd '$ABS' && if [ -d build ]; then find build -maxdepth 1 -type f -perm -u+x -print -delete; fi"
  restart_remote
  exit $?
fi

if (( BUILD )); then
  (cd "$W" && if (( TEST )); then jetson-build --test; else jetson-build; fi)
fi
BIN="$W/build/jetson/install/bin"
shopt -s nullglob
files=("$BIN"/*)
(( ${#files[@]} )) || { echo "no Jetson build in $BIN (run without --no-build)" >&2; exit 1; }
names=()
for f in "${files[@]}"; do names+=("$(basename "$f")"); done

# 임시 폴더에 풀고 mv: 실행 중인 파일도 바꿀 수 있다(돌고 있는 프로세스는 다시 시작할 때까지 예전 파일)
tar -C "$BIN" -cf - "${names[@]}" | "${SSH[@]}" "set -e; d='$ABS/build'; s=\"\$d/.deploy-\$\$\"; mkdir -p \"\$s\";
  tar -C \"\$s\" -xf -; for n in ${names[*]}; do chmod 755 \"\$s/\$n\"; mv -f \"\$s/\$n\" \"\$d/\"; done; rmdir \"\$s\";
  echo \"copied to \$d: ${names[*]}\""
restart_remote
