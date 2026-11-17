#!/bin/bash
# MAVLink 라우터 설치 (FC <-> Jetson <-> 서버). install_workspace.sh가 호출한다. 혼자 실행해도 된다.
# 구성과 FC 설정은 docs/fc-mavlink-router.md.
#
#   sudo bash install_mavlink_router.sh          설치(이미 있으면 건너뜀)
#   bash install_mavlink_router.sh --check       상태만 확인. 아무것도 바꾸지 않는다(root 불필요)
#
# 시스템에 남기는 것: 서비스 파일 1개(/etc/systemd/system/workspace-mavlink-router.service).
# 실행 파일은 워크스페이스(install/bin/mavlink-routerd)에서 바로 실행하고, 설정은 데이터 폴더에 둔다.
#
# 순서:
#   1. 라우터가 이미 설치되어 있으면(수동 설치 포함) 건드리지 않는다. 워크스페이스 출구가 설정에 없으면 경고만
#   2. 실행 파일 sha256 확인
#   3. <데이터>/config/mavlink-router.conf가 없을 때만 기본값으로 만들고, FC가 하트비트를 보내는 포트를 찾아
#      Device·Baud를 채운다. 못 찾으면 기본값(/dev/ttyTHS1, 921600)으로 두고 경고. 있으면 손대지 않는다
#   4. 시리얼 콘솔이 FC 포트를 잡지 않게 끈다
#   5. 서비스 등록·시작
# 재실행해도 된다. 워크스페이스 경로를 옮겼으면 다시 실행한다(우리 서비스 파일의 경로 갱신).
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
WORKSPACE_ROOT="${WORKSPACE_ROOT:-$(cd "$HERE/.." && pwd)}"
WORKSPACE_DATA="${WORKSPACE_DATA:-${WORKSPACE_ROOT}_data}"

UNIT=workspace-mavlink-router.service
UNIT_PATH="/etc/systemd/system/$UNIT"
BIN="$WORKSPACE_ROOT/install/bin/mavlink-routerd"
# 시험한 버전(mavlink-router 5e38c7a, aarch64, JetPack 6에서 빌드). 바꾸면 다시 시험하고 해시를 함께 고친다
BIN_SHA256=8e2cacdc0b7a0daddeb409ca8f155bf530437c91f14b9a626fc412fe0ba08347
CONF="$WORKSPACE_DATA/config/mavlink-router.conf"
CONF_D="$WORKSPACE_DATA/config/mavlink-router.d"
DEFAULT_CONF="$HERE/config/mavlink-router.conf"
LOCAL_PORT=14551

CHECK=no
[[ "${1:-}" == --check ]] && CHECK=yes

log()  { echo "mavlink-router: $*"; }
warn() { echo "mavlink-router: warning: $*" >&2; }

# 워크스페이스 출구(127.0.0.1:14551)가 설정 파일들에 있는지. has_local_endpoint <파일 또는 폴더...>
has_local_endpoint() {
  local f g files=()
  for f in "$@"; do
    if [[ -d "$f" ]]; then
      for g in "$f"/*.conf; do
        if [[ -f "$g" ]]; then files+=("$g"); fi
      done
    elif [[ -f "$f" ]]; then
      files+=("$f")
    fi
  done
  (( ${#files[@]} > 0 )) || return 1
  awk -v port="$LOCAL_PORT" '
    /^[[:space:]]*\[/ { sec = tolower($0); addr = 0; p = 0 }
    sec ~ /udpendpoint/ && tolower($0) ~ /^[[:space:]]*address[[:space:]]*=[[:space:]]*127\.0\.0\.1/ { addr = 1 }
    sec ~ /udpendpoint/ && tolower($0) ~ "^[[:space:]]*port[[:space:]]*=[[:space:]]*" port "[[:space:]]*$" { p = 1 }
    addr && p { found = 1 }
    END { exit !found }' "${files[@]}" 2>/dev/null
}

# ExecStart의 -c, -d 값. unit_conf_paths <서비스 파일>
unit_conf_paths() {
  awk '/^ExecStart=/ {
         for (i = 1; i <= NF; i++) {
           if ($i == "-c" || $i == "--conf-file") print $(i + 1)
           if ($i == "-d" || $i == "--conf-dir") print $(i + 1)
         }
       }' "$1"
}

# ---- 1. 이미 설치되어 있는지 ------------------------------------------------------------
# 우리 서비스가 아닌 mavlink-router 서비스(수동 설치 등)나, 서비스 밖에서 도는 mavlink-routerd
foreign_units=()
declare -A seen_units=()
for f in /etc/systemd/system/*mavlink-router*.service /lib/systemd/system/*mavlink-router*.service \
         /usr/lib/systemd/system/*mavlink-router*.service; do
  [[ -f "$f" ]] || continue
  real="$(readlink -f "$f")"   # /lib -> /usr/lib 링크로 같은 파일이 두 번 잡히지 않게
  if [[ "$(basename "$f")" != "$UNIT" && -z "${seen_units[$real]:-}" ]]; then
    seen_units[$real]=1
    foreign_units+=("$f")
  fi
done
ours_pid="$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null || echo 0)"
foreign_pids=()
while read -r pid; do
  if [[ -n "$pid" && "$pid" != "$ours_pid" ]]; then foreign_pids+=("$pid"); fi
done < <(pgrep -x mavlink-routerd || true)

if (( ${#foreign_units[@]} > 0 || ${#foreign_pids[@]} > 0 )); then
  log "already installed outside the workspace; leaving it as is"
  confs=()
  for f in "${foreign_units[@]}"; do
    log "  service: $f ($(systemctl is-active "$(basename "$f")" 2>/dev/null || true))"
    mapfile -t -O "${#confs[@]}" confs < <(unit_conf_paths "$f")
  done
  for pid in "${foreign_pids[@]}"; do
    log "  process: $pid $(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
  done
  (( ${#confs[@]} > 0 )) || confs=(/etc/mavlink-router/main.conf /etc/mavlink-router/config.d)
  log "  config: ${confs[*]}"
  if has_local_endpoint "${confs[@]}"; then
    log "  workspace endpoint 127.0.0.1:$LOCAL_PORT: present"
  else
    warn "workspace endpoint 127.0.0.1:$LOCAL_PORT is not in ${confs[*]}."
    warn "  workspace programs cannot reach the FC. Add a [UdpEndpoint] (docs/fc-mavlink-router.md)"
  fi
  exit 0
fi

# ---- FC 포트 찾기: 후보 시리얼 포트에서 FC 하트비트(autopilot != INVALID)를 기다린다 -------
# 출력: "<장치> <baud>" 또는 빈 줄. 다른 프로세스가 연 포트는 건너뛴다(데이터를 가로채지 않게)
probe_fc() {
  python3 - <<'PY'
import glob, os, select, termios, time

def busy(dev):
    real = os.path.realpath(dev)
    for fd in glob.glob("/proc/[0-9]*/fd/*"):
        try:
            if os.path.realpath(fd) == real:
                return True
        except OSError:
            pass
    return False

def heartbeat_from_autopilot(buf):
    # MAVLink 2: FD len=9 incompat compat seq sys comp msgid(3)=0  payload: custom_mode(4) type autopilot ...
    # MAVLink 1: FE len=9 seq sys comp msgid=0                     payload: 같음
    for start, hdr, mid in ((b"\xfd\x09", 10, b"\x00\x00\x00"), (b"\xfe\x09", 6, b"\x00")):
        i = buf.find(start)
        while i >= 0:
            frame = buf[i:i + hdr + 9]
            if len(frame) == hdr + 9 and frame[hdr - len(mid):hdr] == mid and frame[hdr + 5] != 8:
                return True
            i = buf.find(start, i + 1)
    return False

def probe(dev, baud):
    try:
        fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError:
        return False
    try:
        old = termios.tcgetattr(fd)
        new = termios.tcgetattr(fd)
        new[0] = new[1] = new[3] = 0
        new[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        new[4] = new[5] = getattr(termios, "B%d" % baud)
        termios.tcsetattr(fd, termios.TCSANOW, new)
        termios.tcflush(fd, termios.TCIFLUSH)
        buf, end = b"", time.time() + 2.5
        while time.time() < end:
            if select.select([fd], [], [], 0.2)[0]:
                try:
                    buf += os.read(fd, 4096)
                except OSError:
                    pass
                if heartbeat_from_autopilot(buf):
                    return True
        return False
    finally:
        try:
            termios.tcsetattr(fd, termios.TCSANOW, old)
        except Exception:
            pass
        os.close(fd)

devs = sorted(glob.glob("/dev/ttyTHS*") + glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"),
              key=lambda d: (d != "/dev/ttyTHS1", d))
for dev in devs:
    if busy(dev):
        continue
    for baud in ((921600,) if "ACM" in dev else (921600, 57600)):
        if probe(dev, baud):
            print(dev, baud)
            raise SystemExit
print()
PY
}

# ---- --check: 상태만 출력 ----------------------------------------------------------------
if [[ "$CHECK" == yes ]]; then
  log "not installed outside the workspace"
  log "service $UNIT: $([[ -f "$UNIT_PATH" ]] && systemctl is-active "$UNIT" 2>/dev/null || echo 'not installed')"
  if [[ -f "$BIN" ]] && [[ "$(sha256sum "$BIN" | cut -d' ' -f1)" == "$BIN_SHA256" ]]; then
    log "binary: $BIN (ok)"
  else
    warn "binary: $BIN missing or checksum mismatch"
  fi
  if [[ -f "$CONF" ]]; then
    log "config: $CONF ($(grep -iE '^[[:space:]]*(Device|Baud)[[:space:]]*=' "$CONF" | tr -s ' ' | paste -sd' '))"
    has_local_endpoint "$CONF" "$CONF_D" && log "workspace endpoint 127.0.0.1:$LOCAL_PORT: present" \
      || warn "workspace endpoint 127.0.0.1:$LOCAL_PORT is not in $CONF"
  else
    log "config: $CONF (not created yet)"
  fi
  # 다른 프로세스가 연 포트인지는 root만 알 수 있다(/proc/<pid>/fd). root가 아니면 찾지 않는다
  if [[ "$(id -u)" -eq 0 ]]; then
    found="$(probe_fc)"
    log "FC heartbeat: ${found:-not found on free ports (FC off, wrong params, or port in use)}"
  else
    log "FC heartbeat: not probed (run with sudo to probe serial ports)"
  fi
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
[[ -f "$BIN" ]] || { echo "missing $BIN" >&2; exit 1; }
if [[ "$(sha256sum "$BIN" | cut -d' ' -f1)" != "$BIN_SHA256" ]]; then
  echo "checksum mismatch: $BIN is not the tested mavlink-router build" >&2
  exit 1
fi
chmod 755 "$BIN"

# ---- 3. 설정(없을 때만) --------------------------------------------------------------------
mkdir -p "$WORKSPACE_DATA/config" "$CONF_D"
chown "$OWNER:$OWNER_GRP" "$WORKSPACE_DATA" "$WORKSPACE_DATA/config" "$CONF_D"
if [[ -f "$CONF" ]]; then
  log "config exists, not changed: $CONF"
else
  cp "$DEFAULT_CONF" "$CONF"
  # 우리 서비스가 FC 포트를 쓰고 있으면 찾을 수 없으므로 잠시 멈춘다(재설치 중 설정 파일을 지운 경우)
  systemctl stop "$UNIT" 2>/dev/null || true
  log "looking for the FC heartbeat on serial ports..."
  found="$(probe_fc)"
  if [[ -n "$found" ]]; then
    read -r dev baud <<< "$found"
    sed -i -E "s|^([[:space:]]*Device[[:space:]]*=).*|\1 $dev|; s|^([[:space:]]*Baud[[:space:]]*=).*|\1 $baud|" "$CONF"
    log "FC found on $dev @ $baud"
  else
    warn "no FC heartbeat found; using default /dev/ttyTHS1 @ 921600."
    warn "  check FC power and params (MAV_1_CONFIG, SER_TEL2_BAUD, MAV_1_FLOW_CTRL=off), then edit $CONF"
  fi
  chown "$OWNER:$OWNER_GRP" "$CONF"
  log "config created: $CONF"
fi
has_local_endpoint "$CONF" "$CONF_D" || warn "workspace endpoint 127.0.0.1:$LOCAL_PORT is not in $CONF"

# ---- 4. 시리얼 콘솔 끄기 -----------------------------------------------------------------
dev="$(awk -F= 'tolower($1) ~ /^[[:space:]]*device[[:space:]]*$/ { gsub(/[[:space:]]/, "", $2); print $2; exit }' "$CONF")"
systemctl disable --now nvgetty.service 2>/dev/null || true
if [[ -n "$dev" ]]; then
  systemctl disable --now "serial-getty@$(basename "$dev").service" 2>/dev/null || true
fi
usermod -aG dialout "$OWNER" || true

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
  log "running: $(journalctl -u "$UNIT" -b --no-pager -o cat 2>/dev/null | grep -m1 'Opened UART' || echo 'see journalctl -u '"$UNIT")"
  log "check FC traffic: journalctl -u $UNIT --since -5s | grep -A9 'flight_controller {'"
else
  echo "mavlink-router: $UNIT is not running. journalctl -u $UNIT -n 50" >&2
  exit 1
fi
