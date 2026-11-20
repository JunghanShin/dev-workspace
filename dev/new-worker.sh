#!/bin/bash
# 새 워커를 워커 틀(workers/_template)로 만든다. PC(WSL)나 보드에서 워크스페이스 폴더 기준으로 실행한다.
#
#   dev/new-worker.sh [--video] <폴더> [type]
#     --video 영상 송출 사본(lib/libvideo)도 복사(영상을 송출하는 장치 워커). 데이터 버스(lib/libframebus)는 늘 복사
#     <폴더>  workers/<폴더>, 워커 id, 실행 파일 <폴더>-worker, 패널 id  (예: thermal)
#     [type]  워커 type (기본 <폴더의 -를 _로>_worker)
#
# 틀의 예제 워커를 이름만 바꿔 복사한다(입출력 정의 io/의 정의 이름도 새 이름으로). 모듈(source·processor·peer)은 이름 그대로 온다:
#   그 자리에 워커의 일을 쓰고, 필요 없는 모듈은 뗀다(틀 README "모듈 만들기·붙이기·떼기").
# 만든 뒤: cd workers/<폴더> && jetson-build --native --test  →  워크스페이스를 다시 시작하면 워커 폴더를 보고 붙는다
set -euo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
VIDEO=0
if [[ "${1:-}" == --video ]]; then VIDEO=1; shift; fi
NAME="${1:-}"
TYPE="${2:-${NAME//-/_}_worker}"
if [[ ! "$NAME" =~ ^[a-z][a-z0-9-]{1,30}$ || ! "$TYPE" =~ ^[a-z][a-z0-9_]{1,40}$ ]]; then
  echo "usage: dev/new-worker.sh [--video] <folder: a-z0-9-> [type]" >&2
  exit 1
fi
T="$ROOT/workers/_template"
W="$ROOT/workers/$NAME"
[[ -e "$W" ]] && { echo "already exists: $W" >&2; exit 1; }

mkdir -p "$W"
# 틀 안내(README.md), new/(문서 틀), python/(Python 워커 도우미 원본), build/는 가져오지 않는다
# io/common.json(공통 정의)은 틀에만 하나(워커는 가져오지 않고 틀의 것을 따름). io/self.json(워커 정의)은 가져와 이 워커 것으로 고쳐 씀
EXCL=(--exclude='./build' --exclude='*/build' --exclude='./README.md' --exclude='./new' --exclude='./python' --exclude='./io/common.json')
(( VIDEO )) || EXCL+=(--exclude='./lib/libvideo')
(cd "$T" && tar "${EXCL[@]}" -cf - .) | (cd "$W" && tar xf -)
for f in README.md design.md; do
  sed -e "s/@NAME@/$NAME/g" -e "s/@TYPE@/$TYPE/g" "$T/new/$f" > "$W/$f"
done
# 실행 구조 한눈 그림의 시작점(예제 워커 그림). 설계하면서 이 워커의 그림으로 바꾼다(설계 가이드 new/GUIDE.md 2장 ④)
cp "$T/new/structure.svg" "$W/structure.svg"

# 워커 이름만 바꾼다(실행 파일, 가짜 장치 프로그램, type, 패널 id). 모듈 이름은 그대로
mv "$W/tools/example-sim.cpp" "$W/tools/$NAME-sim.cpp"
files=("$W/CMakeLists.txt" "$W/src/main.cpp" "$W/test/test_worker.cpp" "$W/workspace/worker-types.json"
       "$W/workspace/presets/sim.json" "$W/tools/$NAME-sim.cpp"
       "$W/workspace/prepare.sh" "$W/workspace/install.sh" "$W/workspace/panel/panel.json" "$W/workspace/panel/panel.js")
for f in "${files[@]}"; do
  sed -i -e "s/example-worker/$NAME-worker/g" -e "s/example-sim/$NAME-sim/g" -e "s/example_worker/$TYPE/g" \
         -e "s/\"id\": \"example\"/\"id\": \"$NAME\"/g" "$f"
done
chmod +x "$W/workspace/prepare.sh" "$W/workspace/install.sh"
sed -i "s/\"title\": \"예제 워커\"/\"title\": \"$NAME\"/" "$W/workspace/panel/panel.json"
# 입출력 정의(워크스페이스 design.md 6.6.7.1): 틀의 정의 "example"을 새 워커 이름의 정의로(역할 이름은 나중에 바꿔도 됨).
# peer 모듈은 같은 종류의 다른 워커를 받으므로 사본도 같은 이름으로
DEF="${NAME//-/_}"
mv "$W/io/example.json" "$W/io/$DEF.json"
mv "$W/io/uses/example.json" "$W/io/uses/$DEF.json"
sed -i -e "s/\"example\"/\"$DEF\"/g" "$W/io.json" "$W/io/$DEF.json" "$W/io/uses/$DEF.json"

echo "created $W (type $TYPE, executable $NAME-worker, modules: source processor peer)"
echo "next:"
echo "  설계서부터: $W/design.md (가이드 workers/_template/new/GUIDE.md: 품질 목표 → 맥락 → 구성 → 실행 구조 그림 structure.svg → io/ → 끊겼을 때 → 시험). 확정 뒤 구현"
echo "  cd $W && jetson-build --native --test"
echo "  모듈 자리에 워커의 일: lib/<모듈>/core(유닛), lib/<모듈>/src/module.cpp(설정·명령·상태·지표·스레드), workspace/(스키마·프리셋·패널)"
echo "  필요 없는 모듈은 CMakeLists.txt·src/main.cpp·workspace/worker-types.json·presets에서 지운다"
(( VIDEO )) && echo "  송출: CMakeLists.txt에 add_subdirectory(lib/libvideo), main.cpp에 host.add<video::StreamsModule>(...) (lib/libvideo/README.md 참고)"
echo "  가짜 장치: tools/$NAME-sim.cpp에 장치 흉내, 프리셋 sim으로 켬(장비 없이 개발)"
echo "  워크스페이스를 다시 시작하면 워커 폴더를 보고 자동으로 붙는다(등록 단계 없음, 처음엔 자동 시작 안 함)"
