#!/bin/bash
# 로컬 미디어 서버(MediaMTX) 설치. install_workspace.sh가 호출한다. 혼자 실행해도 된다.
# 구성은 docs/local-ui-media.md.
#
#   sudo bash install_mediamtx.sh          설치(이미 있으면 건너뜀)
#   bash install_mediamtx.sh --check       상태만 확인. 아무것도 바꾸지 않는다(root 불필요)
#
# 시스템에 남기는 것: 서비스 파일 1개(/etc/systemd/system/workspace-mediamtx.service), ffmpeg(원격 중계용, 없을 때만 apt).
# 실행 파일은 워크스페이스(install/mediamtx/mediamtx, 압축 파일에서 풂)에서 바로 실행하고, 설정은 데이터 폴더에 둔다.
#
# 순서:
#   1. MediaMTX가 이미 설치되어 있거나 8554 포트를 다른 프로그램이 쓰고 있으면 건드리지 않는다
#   2. 압축 파일 sha256 확인(공식 배포 checksums.sha256과 같은 값) → 실행 파일 풀기
#   3. ffmpeg(원격 중계)가 없으면 설치. 실패해도 계속(로컬 시청은 됨)
#   4. <데이터>/config/mediamtx.yml이 없을 때만 기본값 복사. 있으면 손대지 않는다
#   5. 서비스 등록·시작
# 재실행해도 된다. 워크스페이스 경로를 옮겼으면 다시 실행한다(우리 서비스 파일의 경로 갱신).
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
WORKSPACE_ROOT="${WORKSPACE_ROOT:-$(cd "$HERE/.." && pwd)}"
WORKSPACE_DATA="${WORKSPACE_DATA:-${WORKSPACE_ROOT}_data}"

UNIT=workspace-mediamtx.service
UNIT_PATH="/etc/systemd/system/$UNIT"
# 시험한 버전. 바꾸면 다시 시험하고 두 해시를 함께 고친다(압축 파일 해시 = 배포 페이지 checksums.sha256)
VERSION=v1.21.3
TARBALL="$HERE/mediamtx/mediamtx_${VERSION}_linux_arm64.tar.gz"
TARBALL_SHA256=27f038237cb5f77a724f6afc58eca2798f2bc4edf9989895631a80eeb41a4fe2
BIN="$HERE/mediamtx/mediamtx"
BIN_SHA256=a1621ba39c0aa58430809df28263865e37a75bb4d2f1c87e642ddf5f3f67b19d
CONF="$WORKSPACE_DATA/config/mediamtx.yml"
DEFAULT_CONF="$HERE/config/mediamtx.yml"
RTSP_PORT=8554

CHECK=no
[[ "${1:-}" == --check ]] && CHECK=yes

log()  { echo "mediamtx: $*"; }
warn() { echo "mediamtx: warning: $*" >&2; }

sha() { sha256sum "$1" | cut -d' ' -f1; }
bin_ok() { [[ -f "$BIN" ]] && [[ "$(sha "$BIN")" == "$BIN_SHA256" ]]; }

# 원격 중계 대상(설정의 runOnAvailable 끝 주소). relay_target <설정 파일>
relay_target() {
  grep -E '^[[:space:]]*runOnAvailable:' "$1" 2>/dev/null | grep -oE 'rtsps?://[^ ]+$' | grep -v 127.0.0.1 | head -1 || true
}

# ---- 1. 이미 설치되어 있는지 ------------------------------------------------------------
# 우리 서비스가 아닌 MediaMTX 서비스(수동 설치 등), 서비스 밖에서 도는 mediamtx, 8554를 쓰는 다른 프로그램
foreign_units=()
declare -A seen_units=()
for f in /etc/systemd/system/*mediamtx*.service /lib/systemd/system/*mediamtx*.service \
         /usr/lib/systemd/system/*mediamtx*.service; do
  [[ -f "$f" ]] || continue
  real="$(readlink -f "$f")"   # /lib -> /usr/lib 링크로 같은 파일이 두 번 잡히지 않게
  if [[ "$(basename "$f")" != "$UNIT" && -z "${seen_units[$real]:-}" ]]; then
    seen_units[$real]=1
    foreign_units+=("$f")
  fi
done
ours_active=no
systemctl is-active --quiet "$UNIT" 2>/dev/null && ours_active=yes
ours_pid="$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null || echo 0)"
foreign_pids=()
while read -r pid; do
  if [[ -n "$pid" && "$pid" != "$ours_pid" ]]; then foreign_pids+=("$pid"); fi
done < <(pgrep -x mediamtx || true)
port_user=""
if [[ "$ours_active" == no ]] && ss -Hltn "sport = :$RTSP_PORT" 2>/dev/null | grep -q .; then
  port_user="$(ss -Hltnp "sport = :$RTSP_PORT" 2>/dev/null | grep -oE 'users:\(\("[^"]+"' | head -1 | cut -d'"' -f2)"
  port_user="${port_user:-unknown (run with sudo to see)}"
fi

if (( ${#foreign_units[@]} > 0 || ${#foreign_pids[@]} > 0 )) || [[ -n "$port_user" ]]; then
  log "a media server is already installed outside the workspace; leaving it as is"
  for f in "${foreign_units[@]}"; do
    log "  service: $f ($(systemctl is-active "$(basename "$f")" 2>/dev/null || true))"
  done
  for pid in "${foreign_pids[@]}"; do
    log "  process: $pid $(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
  done
  [[ -z "$port_user" ]] || log "  port $RTSP_PORT in use by: $port_user"
  log "  workers can send to rtsp://127.0.0.1:$RTSP_PORT/<path> if that server accepts it."
  log "  remote relay and local WebRTC (:8889) depend on that server's config"
  exit 0
fi

# ---- --check: 상태만 출력 ----------------------------------------------------------------
if [[ "$CHECK" == yes ]]; then
  log "not installed outside the workspace"
  log "service $UNIT: $([[ -f "$UNIT_PATH" ]] && systemctl is-active "$UNIT" 2>/dev/null || echo 'not installed')"
  if bin_ok; then
    log "binary: $BIN ($VERSION, ok)"
  elif [[ -f "$TARBALL" ]]; then
    log "binary: not extracted yet ($TARBALL present)"
  else
    warn "missing $TARBALL"
  fi
  if [[ -f "$CONF" ]]; then
    t="$(relay_target "$CONF")"
    log "config: $CONF (remote relay: ${t:-off})"
  else
    log "config: $CONF (not created yet)"
  fi
  log "ffmpeg (remote relay): $(command -v ffmpeg || echo 'not installed')"
  exit 0
fi

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run as root: sudo bash $0   (status only: bash $0 --check)" >&2
  exit 1
fi
if [[ "$WORKSPACE_ROOT" =~ [[:space:]] || "$WORKSPACE_DATA" =~ [[:space:]] ]]; then
  echo "workspace/data path must not contain spaces" >&2
  exit 1
fi
OWNER="$(stat -c %U "$WORKSPACE_ROOT")"
OWNER_GRP="$(stat -c %G "$WORKSPACE_ROOT")"

# ---- 2. 실행 파일 -----------------------------------------------------------------------
if bin_ok; then
  log "binary ok: $BIN ($VERSION)"
else
  [[ -f "$TARBALL" ]] || { echo "missing $TARBALL" >&2; exit 1; }
  if [[ "$(sha "$TARBALL")" != "$TARBALL_SHA256" ]]; then
    echo "checksum mismatch: $TARBALL is not the official MediaMTX $VERSION release" >&2
    exit 1
  fi
  tar -xzf "$TARBALL" -C "$(dirname "$BIN")" mediamtx
  bin_ok || { echo "extracted $BIN does not match the tested build" >&2; exit 1; }
  chown "$OWNER:$OWNER_GRP" "$BIN"
  log "extracted $BIN ($VERSION)"
fi
chmod 755 "$BIN"

# ---- 3. ffmpeg (원격 중계) -----------------------------------------------------------------
if ! command -v ffmpeg >/dev/null; then
  log "installing ffmpeg (remote relay)"
  export DEBIAN_FRONTEND=noninteractive
  { apt-get update && apt-get install -y ffmpeg; } \
    || warn "could not install ffmpeg (offline?). Local view works; remote relay will not until ffmpeg is installed"
fi

# ---- 4. 설정(없을 때만) --------------------------------------------------------------------
mkdir -p "$WORKSPACE_DATA/config"
chown "$OWNER:$OWNER_GRP" "$WORKSPACE_DATA" "$WORKSPACE_DATA/config"
if [[ -f "$CONF" ]]; then
  log "config exists, not changed: $CONF"
else
  install -m 644 -o "$OWNER" -g "$OWNER_GRP" "$DEFAULT_CONF" "$CONF"
  log "config created: $CONF"
fi
t="$(relay_target "$CONF")"
log "remote relay: ${t:-off} (edit $CONF to change)"

# ---- 5. 서비스 ----------------------------------------------------------------------------
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
sed -e "s|@WORKSPACE_ROOT@|$WORKSPACE_ROOT|g" -e "s|@WORKSPACE_DATA@|$WORKSPACE_DATA|g" -e "s|@WORKSPACE_USER@|$OWNER|g" \
  "$HERE/systemd/$UNIT" > "$tmp"
if [[ -f "$UNIT_PATH" ]] && cmp -s "$tmp" "$UNIT_PATH"; then
  systemctl enable "$UNIT" >/dev/null 2>&1
  if systemctl is-active --quiet "$UNIT"; then
    log "already installed and running: $UNIT (not restarted)"
  else
    systemctl start "$UNIT"
    log "started $UNIT"
  fi
else
  install -m 644 "$tmp" "$UNIT_PATH"
  systemctl daemon-reload
  systemctl enable "$UNIT" >/dev/null 2>&1
  systemctl restart "$UNIT"
  log "installed $UNIT_PATH and started"
fi

sleep 3
if systemctl is-active --quiet "$UNIT"; then
  log "running: $(journalctl -u "$UNIT" -b --no-pager -o cat 2>/dev/null | grep -m1 'RTSP\] started' || echo 'see journalctl -u '"$UNIT")"
  log "workers send to rtsp://127.0.0.1:$RTSP_PORT/<path>; view at http://<jetson>:8889/<path>"
else
  echo "mediamtx: $UNIT is not running. journalctl -u $UNIT -n 50" >&2
  exit 1
fi
