#!/bin/bash
# 커밋 전 검사를 한 번에(PC WSL·Linux). 워크스페이스 폴더 어디서 실행해도 된다.
#
#   dev/check.sh            Python 시험 전부 + 템플릿과 모든 워커 PC 빌드·시험(WORKER_WORKSPACE=ON, OFF)
#   dev/check.sh --quick    Python 시험만(빌드 생략)
#
# Python: manager/test_schema.py(스키마·프리셋), dev/test_workers.py(워커 폴더 구성·사본 라이브러리·패널 id·Python 도우미),
#         rest/test_stream.py(텔레메트리 중계)
# Python 도구: workers/_template/python/test_comm.py(Python ↔ C++ 워커 통신, 틀 빌드 뒤)
# 빌드: workers/_template + workers/*/ 마다 cmake → ctest, 결과는 /tmp/workspace-check/<저장소 폴더>/<워커>-<ON|OFF>
# ThreadSanitizer(제공 도구, design.md 6.6.9 ③): libworker·libframebus 단위·스트레스 시험, 템플릿과 모든 워커의 시험을 TSan 빌드로.
#   워커가 빼려면 그 시험에 CTest 라벨 no_tsan(set_tests_properties(<시험> PROPERTIES LABELS no_tsan))
# Jetson(크로스 빌드 + QEMU) 시험은 확정할 때 워커마다 jetson-build --test (design.md 11.3)
set -uo pipefail
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
QUICK=0
[[ "${1:-}" == --quick ]] && QUICK=1
OUT="/tmp/workspace-check/$(basename "$ROOT")"  # 저장소마다 따로(같은 PC의 다른 워크스페이스와 CMake 캐시가 섞이지 않게)
mkdir -p "$OUT"
fail=()

step() { printf '\n== %s\n' "$*"; }
run_py() {  # run_py <이름> <폴더> <unittest 인자>
  local name=$1 dir=$2 out rc; shift 2
  out="$(cd "$dir" && WORKER_BIN="$OUT/_template-ON/example-worker" PYTHONDONTWRITEBYTECODE=1 python3 -m unittest "$@" 2>&1)"; rc=$?
  echo "$name: $(echo "$out" | grep -E "^Ran " | head -1) $( ((rc)) && echo FAILED || echo OK)"
  ((rc)) && { echo "$out" | grep -E "FAIL:|ERROR:|Error" | head -10; fail+=("$name"); }
  return 0
}

step "Python 시험"
run_py test_schema "$ROOT/manager" test_schema
run_py test_manager_api "$ROOT/manager" test_manager_api
run_py test_workers "$ROOT" dev/test_workers.py
run_py test_stream "$ROOT/rest" test_stream

if (( ! QUICK )); then
  for w in "$ROOT"/workers/*/; do  # _template 포함
    w="${w%/}"
    name="$(basename "$w")"
    [[ -f "$w/CMakeLists.txt" ]] || continue
    for mode in ON OFF; do
      step "$name WORKER_WORKSPACE=$mode"
      b="$OUT/$name-$mode"
      if cmake -S "$w" -B "$b" -DWORKER_WORKSPACE=$mode -DCMAKE_BUILD_TYPE=Release >"$b.cfg.log" 2>&1 \
         && cmake --build "$b" -j"$(nproc)" >"$b.build.log" 2>&1; then
        (cd "$b" && ctest --output-on-failure 2>&1 | grep -E "tests passed|tests failed|FAIL" | head -5)
        (cd "$b" && ctest >/dev/null 2>&1) || fail+=("$name-$mode test")
      else
        echo "build failed: $b.build.log"
        grep -m5 -E "error" "$b.build.log" "$b.cfg.log" 2>/dev/null
        fail+=("$name-$mode build")
      fi
    done
  done
  # Python 워커 통신 도구 ↔ C++ 워커(틀 example-worker): 구독·명령·데이터 버스, 반대 방향(design.md 6.6.7)
  step "Python 통신 도구 ↔ C++"
  run_py test_comm "$ROOT/workers/_template/python" test_comm
  # 데이터 버스 동시성: ThreadSanitizer 빌드로 단위·스트레스 시험(경고가 하나라도 있으면 실패).
  # 최신 커널의 넓은 주소 무작위화와 TSan이 맞지 않아 setarch -R(그 실행만 무작위화 끔)로 돌린다
  tsan="-fsanitize=thread -g -O1"
  # libworker 단위 시험(스레드·큐·단계·잠금)과 템플릿·모든 워커의 시험(연동 ON). 경고가 하나라도 있으면 실패
  for target in "$ROOT/workers/_template/lib/libworker" "$ROOT"/workers/*/; do
    target="${target%/}"
    [[ -f "$target/CMakeLists.txt" ]] || continue
    name="$(basename "$target")"
    [[ "$name" == libworker ]] && name="libworker"
    step "$name ThreadSanitizer"
    b="$OUT/$name-tsan"
    if cmake -S "$target" -B "$b" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="$tsan" -DCMAKE_C_FLAGS="$tsan"          -DCMAKE_EXE_LINKER_FLAGS="$tsan" -DCMAKE_SHARED_LINKER_FLAGS="$tsan" >"$b.cfg.log" 2>&1        && cmake --build "$b" -j"$(nproc)" >"$b.build.log" 2>&1; then
      if (cd "$b" && TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" setarch "$(uname -m)" -R             ctest -LE no_tsan --output-on-failure --timeout 300 >"$b.test.log" 2>&1); then
        echo "$name: OK ($(grep -E "tests passed" "$b.test.log" | tail -1))"
      else
        echo "$name: FAILED ($b.test.log)"
        grep -m8 -E "WARNING: ThreadSanitizer|FAIL|Failed" "$b.test.log"
        fail+=("$name-tsan")
      fi
    else
      echo "build failed: $b.build.log"
      grep -m5 -E "error" "$b.build.log" "$b.cfg.log" 2>/dev/null
      fail+=("$name-tsan build")
    fi
  done
  step "libframebus ThreadSanitizer"
  b="$OUT/libframebus-tsan"
  if cmake -S "$ROOT/workers/_template/lib/libframebus" -B "$b" -DCMAKE_BUILD_TYPE=Debug \
       -DCMAKE_CXX_FLAGS="$tsan" -DCMAKE_EXE_LINKER_FLAGS="$tsan" >"$b.cfg.log" 2>&1 \
     && cmake --build "$b" -j"$(nproc)" >"$b.build.log" 2>&1; then
    for t in test_framebus "test_framebus_stress 2"; do
      if (cd "$b" && TSAN_OPTIONS="halt_on_error=1" setarch "$(uname -m)" -R ./$t >"$b.${t%% *}.log" 2>&1); then
        echo "${t%% *}: OK ($(tail -1 "$b.${t%% *}.log"))"
      else
        echo "${t%% *}: FAILED ($b.${t%% *}.log)"
        grep -m5 -E "WARNING: ThreadSanitizer|FAIL" "$b.${t%% *}.log"
        fail+=("libframebus-tsan ${t%% *}")
      fi
    done
  else
    echo "build failed: $b.build.log"
    fail+=("libframebus-tsan build")
  fi
fi

echo
if (( ${#fail[@]} )); then
  echo "FAILED: ${fail[*]}"
  exit 1
fi
echo "all checks passed"
