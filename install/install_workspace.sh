#!/bin/bash
# 워크스페이스 보드 브링업 (design.md 11.1). 시스템에 남기는 것은 최소한이다:
#   - systemd 서비스 파일 1개(/etc/systemd/system/workspace.service): 부팅 시 워크스페이스의 workspace-start.sh 실행
#   - 로컬 미디어 서버 서비스 파일 1개(/etc/systemd/system/workspace-mediamtx.service): install_mediamtx.sh.
#     이미 설치되어 있으면(수동 설치 포함) 건너뛴다. 실행 파일은 워크스페이스, 설정은 데이터 폴더. 원격 중계용 ffmpeg(없을 때만 apt)
#   - 의존 패키지(없을 때만 apt): python3, openssh-server, util-linux(setpriv), curl
#   - Jetson 전원 모드(nvpmodel): 개발 보드는 최대 성능 MAXN_SUPER로(install/bin/workspace-power-mode)
#   - 워커의 시스템 설치: 고른 워커의 workers/<폴더>/workspace/install.sh (udev 규칙, 패키지, SDK, 워커가 쓰는 서비스)
# 코드·설정·상태·기록은 시스템 폴더(/etc, /opt, /var)에 두지 않는다.
# 워크스페이스와 데이터 폴더(<워크스페이스>_data)에만 있다. 데이터 폴더는 workspace-start.sh가 만든다.
#
#   sudo bash install_workspace.sh                       프로필을 묻는다(Enter = default). 워커는 모두 시스템 설치
#   sudo bash install_workspace.sh --profile <이름>      묻지 않고 install/profiles/<이름>으로(스크립트용)
#   sudo bash install_workspace.sh --profile <폴더>      묻지 않고 그 폴더로(저장소 밖 프로필)
#   sudo bash install_workspace.sh --reset-config        프로필 값으로 보드 설정을 되돌림(기존은 백업)
#   sudo bash install_workspace.sh --power-mode <이름|keep>  전원 모드(기본 MAXN_SUPER, keep = 건드리지 않음)
#
# 프로필 = 프로젝트·현장마다 다른 설정 파일을 모은 폴더. install/profiles/<이름>/에 둔다. 폴더 맨 위의 설정 파일을
# 모두 <데이터>/config/에 복사한다(README·*.md·product.json·하위 폴더 제외). 워크스페이스 파일(network.conf,
# manager.json, workspace.env, mediamtx.yml)과 워커가 쓰는 설정(워커 install.sh가 읽음)을 함께 둔다. 처음 설치할 때만 넣는다: 보드에 이미 그 파일이 있으면(화면·명령으로
# 바꾼 값은 Manager가 이 파일에 바로 저장한다) 그대로 두고, --reset-config일 때만 config/backup-<시각>/에
# 남기고 덮어쓴다. 그 뒤의 설치(워커, 미디어 서버)는 데이터 폴더 설정을 따른다.
# default = 프로필을 복사하지 않음: 데이터 폴더에 있는 설정은 그대로, 없는 파일만 기본값(install/config/)으로
# 만든다.
#
# 워커 = workers/ 아래 workspace/worker-types.json이 있는 폴더. 설치는 워커를 등록하지 않는다:
# 워크스페이스가 시작할 때마다 워커 폴더를 보고 목록을 맞춘다(design.md 6.3.1). 여기서는 워커가 필요로 하는
# 시스템 설치(workspace/install.sh: udev, 패키지, SDK)만 실행한다. 목록을 보여 주고 한 줄로 고른다
# (Enter = 전부, 터미널이 아니면 전부). 시스템 설치가 없는 워커는 건너뛴다.
#
# 재실행해도 된다. 워크스페이스를 다른 경로로 옮겼으면 다시 실행한다(서비스 파일의 경로 갱신).
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
UNIT=workspace.service
# 프로필 폴더의 설정 파일(맨 위 파일만, README·*.md·product.json·숨김 파일 제외). 워커 설정은 워커 폴더(design.md 6.3.1)
profile_files() {
  local f
  for f in "$1"/*; do
    [[ -f "$f" ]] || continue
    case "$(basename "$f")" in README*|*.md|product.json|.*) continue ;; esac
    basename "$f"
  done
}

PROFILES_DIR="$HERE/profiles"

# install/profiles/ 아래 프로필 이름(설정 파일이 하나라도 있는 폴더)
list_profiles() {
  local d
  for d in "$PROFILES_DIR"/*/; do
    [[ -d "$d" ]] || continue
    [[ -n "$(profile_files "${d%/}")" ]] && basename "$d"
  done
}

# 이름 또는 폴더 → 프로필 폴더 절대 경로. default는 빈 문자열(복사 안 함). 없으면 실패
resolve_profile() {
  local p="$1"
  if [[ "$p" == default ]]; then echo ""; return 0; fi
  if [[ "$p" != */* && -d "$PROFILES_DIR/$p" ]]; then (cd "$PROFILES_DIR/$p" && pwd); return 0; fi
  if [[ -d "$p" ]]; then (cd "$p" && pwd); return 0; fi
  return 1
}

PROFILE_ARG=""
RESET_CONFIG=no
POWER_MODE=MAXN_SUPER
while (( $# > 0 )); do
  case "$1" in
    --profile) PROFILE_ARG="${2:?--profile needs a name or folder}"; shift 2 ;;
    --reset-config) RESET_CONFIG=yes; shift ;;
    --power-mode) POWER_MODE="${2:?--power-mode needs a name or keep}"; shift 2 ;;
    *) echo "usage: sudo bash $0 [--profile <name|folder>] [--reset-config] [--power-mode <name|keep>]" >&2; exit 1 ;;
  esac
done

# 워커 폴더의 워크스페이스 연동 파일: workers/<폴더>/workspace/<파일>, 예전 워커는 폴더 맨 위(install.sh는 install/install.sh)
worker_file() {  # worker_file <폴더> <worker-types.json|prepare.sh|install.sh>
  local w="$SRC/workers/$1"
  if [[ -f "$w/workspace/$2" ]]; then echo "$w/workspace/$2"
  elif [[ "$2" == install.sh ]]; then echo "$w/install/install.sh"
  else echo "$w/$2"; fi
}

# workers/ 아래 워커 폴더(worker-types.json이 있는 것. _template 같은 _로 시작하는 폴더는 제외)
list_workers() {
  local d n
  for d in "$SRC"/workers/*/; do
    n="$(basename "$d")"
    [[ "$n" != _* && -f "$(worker_file "$n" worker-types.json)" ]] && echo "$n"
  done
  return 0
}

mapfile -t PROFILE_NAMES < <(list_profiles)
if [[ -z "$PROFILE_ARG" ]]; then
  if [[ -t 0 ]]; then
    echo "프로필: default ${PROFILE_NAMES[*]}"
    echo "  default = 데이터 폴더 설정은 그대로, 없는 것만 기본값"
    while :; do
      read -r -p "선택 (Enter = default): " PROFILE_ARG
      PROFILE_ARG="${PROFILE_ARG:-default}"
      resolve_profile "$PROFILE_ARG" >/dev/null && break
      echo "없는 프로필: $PROFILE_ARG" >&2
    done
  else
    PROFILE_ARG=default   # 터미널이 아니면(스크립트·파이프) 묻지 않음
  fi
fi
PROFILE="$(resolve_profile "$PROFILE_ARG")" || {
  echo "profile not found: $PROFILE_ARG (available: default ${PROFILE_NAMES[*]})" >&2; exit 1; }
echo "profile: $PROFILE_ARG${PROFILE:+ ($PROFILE)}"

# 시스템 설치할 워커: 워커 폴더 전부(묻지 않는다. 워크스페이스는 워커 목록을 들고 있지 않다)
mapfile -t SELECTED < <(list_workers)
echo "workers: ${SELECTED[*]:-(none)}"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run as root: sudo bash $0" >&2
  exit 1
fi
# 서비스 파일의 ExecStart에 경로가 그대로 들어가므로 공백이 있으면 안 된다
if [[ "$SRC" =~ [[:space:]] ]]; then
  echo "workspace path must not contain spaces: $SRC" >&2
  exit 1
fi
owner="$(stat -c %U "$SRC")"
[[ "$owner" != root ]] || echo "warning: workspace is owned by root; programs will run as root" >&2
# 복사 중 실행 권한이 빠져도 ./workspace-start.sh로 바로 실행할 수 있게(서비스는 bash로 실행하므로 무관)
chmod a+x "$SRC/workspace-start.sh" "$SRC/workspace-stop.sh"

# ---- 의존 패키지: 없을 때만 apt로 설치 -----------------------------------------
# 필수: 설치 실패 시 중단. python3(Manager·REST·logger), openssh-server(원격 접속),
# util-linux(setpriv: 시작 셸이 프로그램을 워크스페이스 소유자 계정으로 실행)
REQUIRED_PKGS=(python3 openssh-server util-linux)
# 정비 도구: REST 확인용(curl). 오프라인 현장일 수 있으므로 실패해도 경고만
TOOL_PKGS=(curl openssl)  # openssl: HTTPS 자체 서명 인증서(docs/https.md)

missing_pkgs() {
  local p
  for p in "$@"; do
    dpkg -s "$p" >/dev/null 2>&1 || echo "$p"
  done
}

apt_updated=no
apt_install() {
  export DEBIAN_FRONTEND=noninteractive
  if [[ "$apt_updated" == no ]]; then
    apt-get update || return 1
    apt_updated=yes
  fi
  apt-get install -y "$@"
}

mapfile -t need < <(missing_pkgs "${REQUIRED_PKGS[@]}")
if (( ${#need[@]} > 0 )); then
  echo "installing required packages: ${need[*]}"
  apt_install "${need[@]}" || { echo "failed to install ${need[*]}" >&2; exit 1; }
fi
mapfile -t need < <(missing_pkgs "${TOOL_PKGS[@]}")
if (( ${#need[@]} > 0 )); then
  echo "installing tools: ${need[*]}"
  apt_install "${need[@]}" || echo "warning: could not install ${need[*]} (offline?), continuing" >&2
fi
# 부팅 시 sshd가 뜨게(기본 22). 워크스페이스는 sshd 설정을 바꾸지 않는다
systemctl enable --now ssh.service
# 고정 IP 적용에 필요. 네트워크 구성을 바꾸므로 자동 설치하지 않는다
command -v nmcli >/dev/null || echo "warning: nmcli not found; network.conf cannot be applied" >&2

# ---- 전원 모드: 개발 보드는 최대 성능(기본 MAXN_SUPER). 제품은 제품 빌드에서 고른 모드를 제품 설치가 맞춘다 ----
if [[ "$POWER_MODE" != keep ]]; then
  bash "$HERE/bin/workspace-power-mode" "$POWER_MODE" \
    || echo "warning: power mode $POWER_MODE not applied (see above; --power-mode keep to skip)" >&2
fi

# ---- 프로필: 프로젝트·현장 설정을 데이터 폴더에 넣는다 ------------------------------------
DATA="${SRC}_data"
OWNER_GRP="$(stat -c %G "$SRC")"
changed=()   # 프로필로 내용이 바뀐 설정 파일(설치 끝에 해당 서비스를 다시 시작)
if [[ -n "$PROFILE" ]]; then
  echo "==== profile: $PROFILE"
  mkdir -p "$DATA/config"
  chown "$owner:$OWNER_GRP" "$DATA" "$DATA/config"
  backup="$DATA/config/backup-$(date +%Y%m%d-%H%M%S)"
  copied=0
  kept=()
  while read -r f; do
    [[ -n "$f" ]] || continue
    copied=$((copied + 1))
    dst="$DATA/config/$f"
    cmp -s "$PROFILE/$f" "$dst" 2>/dev/null && continue   # 이미 같음
    # 보드 값이 있으면 그대로(처음 시작할 때 채운 기본값과 같으면 아직 안 바꾼 것으로 본다)
    if [[ -f "$dst" && "$RESET_CONFIG" == no ]] && ! cmp -s "$dst" "$HERE/config/$f" 2>/dev/null; then
      kept+=("$f")
      continue
    fi
    if [[ -f "$dst" ]]; then
      mkdir -p "$backup"
      cp -p "$dst" "$backup/"
    fi
    install -m 644 -o "$owner" -g "$OWNER_GRP" "$PROFILE/$f" "$dst"
    changed+=("$f")
    echo "  $f"
  done < <(profile_files "$PROFILE")
  (( copied > 0 )) || echo "warning: no profile files in $PROFILE" >&2
  (( ${#kept[@]} == 0 )) || echo "  보드 값 유지(프로필과 다름): ${kept[*]}  (프로필로 되돌리려면 --reset-config)"
  [[ ! -d "$backup" ]] || { chown -R "$owner:$OWNER_GRP" "$backup"; echo "  previous files: $backup"; }
  # 어느 프로필을 언제 넣었는지 기록(화면 설정 > 워크스페이스, REST GET /system/workspace)
  python3 - "$DATA/config/profile.json" "$PROFILE_ARG" "$PROFILE" "$SRC" "$RESET_CONFIG"       "$(git -c safe.directory="$SRC" -C "$SRC" rev-parse --short HEAD 2>/dev/null || true)"       "${changed[*]}" "${kept[*]}" <<'PY'
import datetime, json, os, sys
out, name, folder, src, reset, commit, changed, kept = sys.argv[1:9]
rel = os.path.relpath(folder, src)
json.dump({
    "name": os.path.basename(folder.rstrip("/")) if name.find("/") >= 0 else name,
    "source": folder if rel.startswith("..") else rel,
    "applied_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
    "workspace_commit": commit or None,
    "reset_config": reset == "yes",
    "copied": changed.split(),
    "kept_board_values": kept.split(),
}, open(out, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
PY
  chown "$owner:$OWNER_GRP" "$DATA/config/profile.json"
  echo "  기록: $DATA/config/profile.json"
fi

# ---- 워커의 시스템 설치: workers/<폴더>/workspace/install.sh ------------------------------------
# 워커를 등록하지 않는다(워크스페이스가 시작할 때 워커 폴더를 보고 목록을 맞춘다). 설치 내용은 워커 폴더가 갖는다.
# 하나가 실패해도 나머지는 설치하고, 끝에 실패 목록을 알린다
failed_workers=()
for name in "${SELECTED[@]}"; do
  script="$(worker_file "$name" install.sh)"
  echo "==== worker: $name"
  if [[ ! -f "$script" ]]; then
    echo "no system install for $name; skipped"
    continue
  fi
  WORKSPACE_ROOT="$SRC" WORKSPACE_DATA="$DATA" bash "$script" || failed_workers+=("$name")
done
# 기본 탑재 워커(플랫폼 부품 맨 위 worker-types.json, 예: monitor/)의 "requires" 도구가 없으면 알린다
python3 - "$SRC" <<'PY'
import glob, json, os, shutil, sys
for path in sorted(glob.glob(os.path.join(sys.argv[1], "*", "worker-types.json"))):
    try:
        entries = json.load(open(path))
    except (OSError, ValueError):
        continue
    for name, t in entries.items():
        if isinstance(t, dict) and isinstance(t.get("builtin"), dict):
            missing = [x for x in t.get("requires", []) if not (shutil.which(x) or os.path.exists("/usr/bin/" + x))]
            if missing:
                print("warning: built-in %r disabled on this board (missing: %s)" % (name, ", ".join(missing)), file=sys.stderr)
PY

# ---- 로컬 미디어 서버 MediaMTX (워커 영상 로컬 시청 + 원격 중계, docs/local-ui-media.md) ---------------
# 이미 설치되어 있으면 건너뛴다. 실패해도 워크스페이스 설치는 계속하고 끝에 알린다
echo "==== mediamtx"
mediamtx_failed=no
WORKSPACE_ROOT="$SRC" WORKSPACE_DATA="$DATA" bash "$HERE/install_mediamtx.sh" || mediamtx_failed=yes

# 프로필이 미디어 서버 설정을 바꿨으면, 이미 돌고 있던 서비스에 적용(워커가 쓰는 서비스는 워커 install.sh가 맡음)
for f in "${changed[@]}"; do
  case "$f" in
    mediamtx.yml) systemctl try-restart workspace-mediamtx.service 2>/dev/null || true ;;
  esac
done

# ---- systemd 서비스 파일 1개 ----------------------------------------------------------
sed "s|@WORKSPACE_ROOT@|$SRC|g" "$HERE/systemd/$UNIT" > "/etc/systemd/system/$UNIT"
chmod 644 "/etc/systemd/system/$UNIT"
systemctl daemon-reload
systemctl enable "$UNIT"

echo "service: /etc/systemd/system/$UNIT -> $SRC/workspace-start.sh (no automatic restart)"
echo "data:    ${SRC}_data (created by workspace-start.sh)"
echo "start:   $SRC/workspace-start.sh    stop: $SRC/workspace-stop.sh"
if systemctl is-active --quiet "$UNIT"; then
  echo "running: 설치 결과(워커, 설정)는 다시 시작해야 반영됩니다: $SRC/workspace-start.sh"
fi
# 화면 코드(ui/web). 없으면 REST의 화면 제공만 안 된다(API는 됨)
if [[ -f "$SRC/ui/web/shell.html" ]]; then
  echo "ui:      http://<this board>:8080 (served by REST)"
else
  echo "warning: UI files missing in $SRC/ui/web (workspace copy incomplete?)" >&2
fi
failed=no
if [[ "$mediamtx_failed" == yes ]]; then
  echo "FAILED: mediamtx (fix, then: sudo bash $HERE/install_mediamtx.sh)" >&2
  failed=yes
fi
if (( ${#failed_workers[@]} > 0 )); then
  echo "FAILED workers: ${failed_workers[*]} (fix, then: sudo bash $SRC/workers/<name>/workspace/install.sh)" >&2
  failed=yes
fi
[[ "$failed" == no ]] || exit 1
echo "Done."
