#!/bin/bash
# C++ 워커의 Jetson용 실행 파일을 저장소에 넣는다(workers/<워커>/workspace/bin/, 예전 워커는 bin/). PC(WSL)에서 실행한다.
# 새 보드는 저장소를 받아 설치만 하면 이 실행 파일로 바로 돈다(빌드 환경 불필요).
#
#   dev/release-worker.sh <워커>     지금 소스로 jetson-build --test(빌드 + QEMU 시험) → build/jetson/install/bin/*
#                                    → workers/<워커>/workspace/bin/ + BUILD, git에 올려 둠(exec 비트)
#   dev/release-worker.sh manager    플랫폼 process-manager(C++) → manager/bin/ (같은 방식)
#   dev/release-worker.sh gateway    플랫폼 게이트웨이(C++) → gateway/bin/
#   dev/release-worker.sh monitor    플랫폼 시스템 모니터 sysmon(C++, 기본 탑재 워커) → monitor/bin/
#   git commit
#
# 빌드를 이 스크립트가 직접 한다: 따로 빌드하게 두면 빌드가 실패하거나 안 돌았을 때(예: jetson-build를 못 찾음)
# 예전 빌드 결과가 그대로 복사돼, BUILD의 소스 커밋과 실제 실행 파일이 어긋났다. 빌드는 증분이라 이미 빌드했으면 시험만 다시 돈다.
# 빌드나 시험이 실패하면 아무것도 바꾸지 않고 멈춘다.
#
# BUILD에는 소스 커밋, 빌드 환경 버전, 파일별 sha256을 적는다. 커밋하지 않은 소스 변경이 있으면 멈춘다
# (어느 소스로 만든 실행 파일인지 알 수 없게 되므로). 그래도 넣으려면 --dirty.
set -euo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
name=""
dirty_ok=no
for a in "$@"; do
  case "$a" in
    --dirty) dirty_ok=yes ;;
    -*) name="" ; break ;;
    *) name="$a" ;;
  esac
done
if [[ "$name" == manager || "$name" == gateway || "$name" == monitor ]]; then  # 플랫폼 블록(워커 폴더가 아님)
  W="$ROOT/$name"
  SRCDIR="$name"
else
  [[ -n "$name" && -d "$ROOT/workers/$name" ]] || { echo "usage: $0 <worker folder>|manager|gateway|monitor [--dirty]  (e.g. replay)" >&2; exit 1; }
  W="$ROOT/workers/$name"
  SRCDIR="workers/$name"
fi
SRC_BIN="$W/build/jetson/install/bin"
DST="$W/bin"
[[ -d "$W/workspace" ]] && DST="$W/workspace/bin"  # 워크스페이스 연동 파일 폴더가 있으면 그 안에(템플릿 구성)
REL="${DST#$ROOT/}"

# 소스(bin/ 제외, lib/ 포함)에 커밋 안 된 변경이 있는지(빌드 전에: 오래 기다린 뒤 멈추지 않게)
changes="$(git -C "$ROOT" status --porcelain -- "$SRCDIR" ":!$REL" 2>/dev/null || true)"
if [[ -n "$changes" && "$dirty_ok" == no ]]; then
  echo "uncommitted source changes; commit first (or --dirty):" >&2
  echo "$changes" >&2
  exit 1
fi

command -v jetson-build >/dev/null 2>&1 || {
  echo "jetson-build not found in PATH (WSL: run from a login shell, e.g. 'bash -l', or install jetson-dev-env)" >&2
  exit 1
}
# 설치 결과 폴더를 비우고 빌드한다: 빌드 폴더는 증분이라, 이름을 바꾸거나 지운 실행 파일의 예전 결과가
# install/bin에 남아 함께 확정되던 일이 있었다. 비워 두면 지금 CMake 설치 규칙에 있는 실행 파일만 다시 설치된다
rm -rf "$SRC_BIN"
echo "== jetson-build --test ($name)"
(cd "$W" && jetson-build --test) || { echo "jetson-build failed; nothing released" >&2; exit 1; }

ls "$SRC_BIN"/* >/dev/null 2>&1 || { echo "no build output in $SRC_BIN (CMake install rules?)" >&2; exit 1; }
for f in "$SRC_BIN"/*; do
  file -b "$f" | grep -q 'ARM aarch64' || { echo "$f is not an aarch64 executable" >&2; exit 1; }
done

mkdir -p "$DST"
rm -f "$DST"/*
cp "$SRC_BIN"/* "$DST/"
chmod 755 "$DST"/*
{
  echo "# $name 워커 실행 파일(Jetson aarch64). dev/release-worker.sh가 만든다. 손으로 고치지 않는다"
  echo "source_commit: $(git -C "$ROOT" rev-parse --short HEAD)$([[ -n "$changes" ]] && echo ' (dirty)')"
  echo "build_env: jetson-dev $(cat "$ROOT/.jetson-dev-env" 2>/dev/null || echo '?')"
  echo "built: $(date -r "$SRC_BIN/$(ls "$SRC_BIN" | head -1)" '+%Y-%m-%d %H:%M')"  # 링크한 시각(바뀌지 않았으면 그 전 빌드)
  echo "sha256:"
  (cd "$DST" && sha256sum $(ls | grep -v '^BUILD$') | sed 's/^/  /')
} > "$DST/BUILD"
cat "$DST/BUILD"
echo
# 실행 권한을 git에 기록한다(Windows에서 커밋하면 빠져서 보드에서 실행 파일로 쓰지 못함)
git -C "$ROOT" add "$REL"
git -C "$ROOT" update-index --chmod=+x $(cd "$ROOT" && git ls-files "$REL" | grep -v BUILD)
echo "next: git -C $ROOT commit   ($REL is staged with the exec bit)"
