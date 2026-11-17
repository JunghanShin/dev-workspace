#!/bin/bash
# 워커 1회 설치(워크스페이스 연동). install_workspace.sh가 이 워커를 고르면 root로 실행한다.
# 시스템에 넣을 것(패키지, udev 규칙, SDK)만 여기에 둔다. 다시 실행해도 같은 결과가 되게 쓴다.
# 예제 워커는 설치할 것이 없다. 실행 파일은 저장소의 workspace/bin/(확정본)을 그대로 쓴다.
#
# 쓸 수 있는 환경 변수: WORKSPACE_ROOT(워크스페이스 폴더), WORKSPACE_DATA(데이터 폴더)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
NAME="$(basename "$(dirname "$HERE")")"
EXE="replay-worker"

if [[ -x "$HERE/bin/$EXE" ]]; then
  echo "$EXE: $HERE/bin/$EXE ($(grep -m1 '^source_commit' "$HERE/bin/BUILD" 2>/dev/null || echo 'no BUILD'))"
else
  echo "warning: $NAME: no $HERE/bin/$EXE yet (release it: dev/release-worker.sh $NAME)" >&2
fi
echo "Done."
