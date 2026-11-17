#!/bin/bash
# 워크스페이스 전체 종료 (design.md 11.2). root가 아니면 sudo로 다시 실행한다.
#
#   ./workspace-stop.sh
#
# workspace-start.sh(감시 셸)가 REST → Manager(워커 정리) → logger 순서로 내린다.
# 자동 재실행은 없다. 다시 띄우려면 ./workspace-start.sh
# 같은 부팅에서 다시 시작하면 워커는 멈추기 직전 실행 상태로 돌아온다.
set -uo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
DATA="${WORKSPACE_DATA:-${ROOT}_data}"
UNIT=workspace.service

[[ "$(id -u)" -eq 0 ]] || exec sudo bash "$0"

if systemctl is-active --quiet "$UNIT"; then
  systemctl stop "$UNIT"
  systemctl --no-pager status "$UNIT" --lines=15 || true
  exit 0
fi

# 서비스 없이(--foreground) 띄운 경우
pidfile="$DATA/run/workspace-start.pid"
if [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
  pid="$(cat "$pidfile")"
  kill -TERM "$pid"
  for (( i = 0; i < 400; i++ )); do
    kill -0 "$pid" 2>/dev/null || { echo "workspace-stop: stopped"; exit 0; }
    sleep 0.1
  done
  echo "workspace-stop: still running after 40s (pid $pid)" >&2
  exit 1
fi
echo "workspace-stop: not running"
