#!/bin/bash
# 워커 준비(워크스페이스 연동). workspace-start.sh가 시작할 때마다 워커 폴더마다 실행한다.
# 결과: <데이터>/config/worker-types.d/<워커 폴더>.json (type 정의 + 이 보드에서 쓸 실행 파일 경로)
#
# 실행 파일. 보드 구조에 맞는 것을 고른다(개발 중 올린 것이 우선):
#   aarch64(Jetson): build/<실행 파일>(jetson-deploy로 복사) → build/jetson/install/bin/ → workspace/bin/(확정, dev/release-worker.sh)
#   x86_64(PC 개발):  build/native/install/bin/<실행 파일>(jetson-build --native)
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"   # workers/<워커>/workspace
W="$(dirname "$HERE")"                   # workers/<워커>
NAME="$(basename "$W")"
EXE="replay-worker"                     # 워커 실행 파일 이름(CMakeLists의 add_executable)
: "${WORKSPACE_ROOT:?}" "${WORKSPACE_DATA:?}"

case "$(uname -m)" in
    aarch64) candidates=("$W/build/$EXE" "$W/build/jetson/install/bin/$EXE" "$HERE/bin/$EXE") ;;
    *) candidates=("$W/build/native/install/bin/$EXE") ;;
esac
bin=""
for c in "${candidates[@]}"; do
    if [[ -x "$c" ]]; then bin="$c"; break; fi
done
if [[ -z "$bin" ]]; then
    # type은 등록해 두고, 워커를 띄우면 실행 파일이 없다는 오류로 끝난다
    bin="${candidates[0]}"
    echo "$NAME prepare: no $EXE binary for $(uname -m) (build it, then jetson-deploy to $W/build)"
fi
[[ ! -x "$bin" || "$bin" == "$HERE/bin/"* ]] || echo "$NAME prepare: using development build $bin (delete it to use workspace/bin/)"
# type 조각: 실행 파일 자리(@WORKER_EXEC@)를 지금 고른 것으로, 후보 목록(exec_candidates)도 함께 적는다.
# Manager는 워커를 띄울 때마다 후보 중 있는 첫째를 쓴다 → build/에 개발 빌드를 올리고 그 워커만 다시 시작하면 반영
# (dev/deploy-worker.sh). build/의 것을 지우고 다시 시작하면 확정(workspace/bin/)으로 돌아간다
python3 - "$HERE/worker-types.json" "$bin" "${candidates[@]}" > "$WORKSPACE_DATA/config/worker-types.d/$NAME.json" <<'PY'
import json, sys
src, chosen, cands = sys.argv[1], sys.argv[2], sys.argv[3:]
types = json.load(open(src, encoding="utf-8"))
for t in types.values():
    if isinstance(t, dict):
        t["exec"] = [chosen if a == "@WORKER_EXEC@" else a for a in t.get("exec", [])]
        t["exec_candidates"] = cands
json.dump(types, sys.stdout, ensure_ascii=False, indent=2)
PY
