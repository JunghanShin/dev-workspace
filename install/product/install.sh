#!/bin/bash
# 제품 묶음 설치(장비에서 root로). 제품 빌드(화면 또는 dev/build-product.sh)가 만든 묶음 안에 들어간다(워크스페이스 design.md 11.4).
#
#   sudo bash install.sh [--prefix DIR] [--owner USER] [--no-start]   설치(또는 새 버전으로 갱신)
#   sudo bash install.sh --rollback [--prefix DIR]                     바로 전 버전으로 되돌림
#   sudo bash install.sh --status [--prefix DIR]                       설치된 버전·서비스 상태
#   sudo bash install.sh --uninstall [--purge] [--prefix DIR]          서비스·실행 파일 제거(--purge면 데이터도)
#
# 놓는 곳(기본 PREFIX=/opt/<제품 이름>):
#   PREFIX/releases/<버전>/   이 묶음(읽기 전용, root 소유)          PREFIX/current → 쓰는 버전(링크)
#   PREFIX/data/              설정·상태·로그·기록(워커 실행 계정 소유). 처음 설치할 때 프로필로 채우고 장비별 토큰을 만든다
#   /etc/systemd/system/<이름>.service   자동 시작·자동 재시작(Restart=always), 제품 모드 환경
#   전원 모드: 제품 빌드에서 고른 모드(PRODUCT_POWER_MODE, keep이면 건드리지 않음)를 맞춘다
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
NAME="$(cat "$HERE/PRODUCT_NAME")"
VERSION="$(cat "$HERE/VERSION")"
PREFIX="/opt/$NAME"
OWNER="${SUDO_USER:-}"
ACTION=install START=1 PURGE=0
while (( $# )); do
  case "$1" in
    --prefix) PREFIX="$2"; shift ;;
    --owner) OWNER="$2"; shift ;;
    --no-start) START=0 ;;
    --rollback) ACTION=rollback ;;
    --status) ACTION=status ;;
    --uninstall) ACTION=uninstall ;;
    --purge) PURGE=1 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
UNIT="$NAME.service"
UNIT_FILE="/etc/systemd/system/$UNIT"
DATA="$PREFIX/data"
log() { echo "$NAME install: $*"; }
[[ "$(id -u)" -eq 0 ]] || { echo "run as root: sudo bash $0 $*" >&2; exit 1; }

case "$ACTION" in
  status)
    echo "current: $(readlink "$PREFIX/current" 2>/dev/null || echo none)"
    echo "previous: $(cat "$PREFIX/previous" 2>/dev/null || echo none)"
    ls "$PREFIX/releases" 2>/dev/null | sed 's/^/release: /'
    systemctl --no-pager status "$UNIT" --lines=5 || true
    exit 0 ;;
  rollback)
    prev="$(cat "$PREFIX/previous" 2>/dev/null || true)"
    [[ -n "$prev" && -d "$PREFIX/releases/$prev" ]] || { log "no previous release"; exit 1; }
    cur="$(basename "$(readlink "$PREFIX/current")")"
    ln -sfn "releases/$prev" "$PREFIX/current.new" && mv -T "$PREFIX/current.new" "$PREFIX/current"
    echo "$cur" > "$PREFIX/previous"
    log "rolled back to $prev (previous: $cur)"
    systemctl restart "$UNIT"
    exit 0 ;;
  uninstall)
    systemctl disable --now "$UNIT" 2>/dev/null || true
    rm -f "$UNIT_FILE"
    systemctl daemon-reload
    rm -rf "$PREFIX/releases" "$PREFIX/current" "$PREFIX/previous"
    if (( PURGE )); then rm -rf "$DATA"; rmdir "$PREFIX" 2>/dev/null || true; log "removed with data"
    else log "removed (data kept: $DATA)"; fi
    exit 0 ;;
esac

# ---- 설치·갱신 -----------------------------------------------------------------------------
[[ -n "$OWNER" && "$OWNER" != root ]] || { log "give the account that runs the workers: --owner USER"; exit 1; }
id "$OWNER" >/dev/null || { log "no such user: $OWNER"; exit 1; }
( cd "$HERE" && sha256sum -c --quiet SHA256SUMS ) || { log "checksum mismatch: bundle is damaged"; exit 1; }
POWER_MODE="$(cat "$HERE/PRODUCT_POWER_MODE" 2>/dev/null || echo keep)"
if [[ "$POWER_MODE" != keep ]]; then  # 설치 전에: 이 장비에 없는 모드면 아무것도 바꾸지 않고 멈춘다
  bash "$HERE/install/bin/workspace-power-mode" --check "$POWER_MODE" \
    || { log "power mode $POWER_MODE is not available on this device (bundle built for another module?)"; exit 1; }
fi

REL="$PREFIX/releases/$VERSION"
mkdir -p "$PREFIX/releases"
rm -rf "$REL.tmp"
cp -a "$HERE/." "$REL.tmp"
chown -R root:root "$REL.tmp"
chmod -R go-w "$REL.tmp"
rm -rf "$REL"
mv "$REL.tmp" "$REL"
log "release $VERSION -> $REL"

# 데이터 폴더: 처음이면 프로필 파일, 장비별 토큰(REST 인증). 있으면 그대로 둔다(현장 값 유지)
mkdir -p "$DATA/config"
for f in "$REL"/profile/*; do
  [[ -e "$f" ]] || continue
  [[ -e "$DATA/config/$(basename "$f")" ]] || cp "$f" "$DATA/config/"
done
ENV_FILE="$DATA/config/workspace.env"
touch "$ENV_FILE"
if ! grep -q '^WORKSPACE_REST_TOKEN=' "$ENV_FILE"; then
  TOKEN="$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')"
  printf 'WORKSPACE_REST_TOKEN=%s\n' "$TOKEN" >> "$ENV_FILE"
  log "REST token created (keep it): $TOKEN"
fi
grep -q '^WORKSPACE_UI=' "$ENV_FILE" || printf 'WORKSPACE_UI=%s\n' "$(cat "$REL/PRODUCT_UI")" >> "$ENV_FILE"
chmod 600 "$ENV_FILE"
chown -R "$(id -u "$OWNER"):$(id -g "$OWNER")" "$DATA"

# 워커 시스템 설치(udev, SDK 등). 워커 폴더의 install.sh를 제품 경로로
for inst in "$REL"/workers/*/workspace/install.sh; do
  [[ -f "$inst" ]] || continue
  w="$(basename "$(dirname "$(dirname "$inst")")")"
  if WORKSPACE_ROOT="$REL" WORKSPACE_DATA="$DATA" WORKSPACE_OWNER="$OWNER" bash "$inst"; then log "worker $w installed"
  else log "warning: worker $w install failed"; fi
done

# 전원 모드(제품 빌드에서 고름)
if [[ "$POWER_MODE" != keep ]]; then
  bash "$REL/install/bin/workspace-power-mode" "$POWER_MODE" | sed "s/^/$NAME install: /"
fi

# 서비스: 제품 모드(설정 잠금 여부는 묶음이 정함), 자동 재시작
cat > "$UNIT_FILE" <<UNIT
[Unit]
Description=$NAME (product $VERSION)
Wants=network-online.target
After=network-online.target NetworkManager.service

[Service]
# 워커 스레드 실시간 우선순위·워커 nice 낮추기 허용(제공 도구, design.md 6.6.9). 일반 계정으로 낮춰도 이어짐
LimitRTPRIO=95
LimitNICE=40
Type=simple
Environment=WORKSPACE_DATA=$DATA
Environment=WORKSPACE_UNIT=$UNIT
Environment=WORKSPACE_OWNER=$OWNER
Environment=WORKSPACE_PRODUCT=1
Environment=WORKSPACE_CONFIG_WRITE=$(cat "$REL/PRODUCT_CONFIG_WRITE")
ExecStart=/bin/bash $PREFIX/current/workspace-start.sh --service
KillMode=mixed
TimeoutStopSec=45
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
UNIT

# 쓰는 버전 바꾸기(링크를 한 번에). 전 버전은 되돌리기용으로 기록
if [[ -L "$PREFIX/current" ]]; then
  cur="$(basename "$(readlink "$PREFIX/current")")"
  [[ "$cur" != "$VERSION" ]] && echo "$cur" > "$PREFIX/previous"
fi
ln -sfn "releases/$VERSION" "$PREFIX/current.new" && mv -T "$PREFIX/current.new" "$PREFIX/current"
systemctl daemon-reload
systemctl enable "$UNIT" >/dev/null
if (( START )); then systemctl restart "$UNIT"; log "started $UNIT"; else log "installed (not started): systemctl start $UNIT"; fi
log "done: $NAME $VERSION (rollback: sudo bash $REL/install.sh --rollback --prefix $PREFIX)"
