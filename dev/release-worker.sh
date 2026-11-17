#!/bin/bash
# C++ 워커의 Jetson용 실행 파일을 저장소에 넣는다(workers/<워커>/workspace/bin/, 예전 워커는 bin/). PC(WSL)에서 실행한다.
# 새 보드는 저장소를 받아 설치만 하면 이 실행 파일로 바로 돈다(빌드 환경 불필요).
#
#   cd workers/<워커> && jetson-build --test     먼저 빌드·시험
#   dev/release-worker.sh <워커>                 build/jetson/install/bin/* → workers/<워커>/bin/ + BUILD
#   git add workers/<워커>/bin && git commit
#
# BUILD에는 소스 커밋, 빌드 환경 버전, 파일별 sha256을 적는다. 커밋하지 않은 소스 변경이 있으면 멈춘다
# (어느 소스로 만든 실행 파일인지 알 수 없게 되므로). 그래도 넣으려면 --dirty.
set -euo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
name="${1:-}"
[[ -n "$name" && -d "$ROOT/workers/$name" ]] || { echo "usage: $0 <worker folder> [--dirty]  (e.g. replay)" >&2; exit 1; }
dirty_ok=no
[[ "${2:-}" == --dirty ]] && dirty_ok=yes

W="$ROOT/workers/$name"
SRC_BIN="$W/build/jetson/install/bin"
DST="$W/bin"
[[ -d "$W/workspace" ]] && DST="$W/workspace/bin"  # 워크스페이스 연동 파일 폴더가 있으면 그 안에(템플릿 구성)
REL="${DST#$ROOT/}"
ls "$SRC_BIN"/* >/dev/null 2>&1 || { echo "no build output in $SRC_BIN. Run: cd $W && jetson-build --test" >&2; exit 1; }

for f in "$SRC_BIN"/*; do
  file -b "$f" | grep -q 'ARM aarch64' || { echo "$f is not an aarch64 executable" >&2; exit 1; }
done

# 워커 소스(bin/ 제외, lib/ 포함)에 커밋 안 된 변경이 있는지
changes="$(git -C "$ROOT" status --porcelain -- "workers/$name" ":!$REL" 2>/dev/null || true)"
if [[ -n "$changes" && "$dirty_ok" == no ]]; then
  echo "uncommitted source changes; commit first (or --dirty):" >&2
  echo "$changes" >&2
  exit 1
fi

mkdir -p "$DST"
rm -f "$DST"/*
cp "$SRC_BIN"/* "$DST/"
chmod 755 "$DST"/*
{
  echo "# $name 워커 실행 파일(Jetson aarch64). dev/release-worker.sh가 만든다. 손으로 고치지 않는다"
  echo "source_commit: $(git -C "$ROOT" rev-parse --short HEAD)$([[ -n "$changes" ]] && echo ' (dirty)')"
  echo "build_env: jetson-dev $(cat "$ROOT/.jetson-dev-env" 2>/dev/null || echo '?')"
  echo "built: $(date -r "$SRC_BIN/$(ls "$SRC_BIN" | head -1)" '+%Y-%m-%d %H:%M')"
  echo "sha256:"
  (cd "$DST" && sha256sum $(ls | grep -v '^BUILD$') | sed 's/^/  /')
} > "$DST/BUILD"
cat "$DST/BUILD"
echo
# 실행 권한을 git에 기록한다(Windows에서 커밋하면 빠져서 보드에서 실행 파일로 쓰지 못함)
git -C "$ROOT" add "$REL"
git -C "$ROOT" update-index --chmod=+x $(cd "$ROOT" && git ls-files "$REL" | grep -v BUILD)
echo "next: git -C $ROOT commit   ($REL is staged with the exec bit)"
