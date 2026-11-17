"""Manager 설정·운용 모드·워커 type 레지스트리 로딩.

기본값은 design.md 14장 초기 설정값. 모든 값은 설정 파일로 덮어쓸 수 있다.
경로는 데이터 폴더(WORKSPACE_DATA, design.md 11장)에서 정한다. 시스템 경로(/etc, /var, /run)는 쓰지 않는다.
"""
import copy
import json
import logging
import os

log = logging.getLogger("workspace.manager")

# 데이터 폴더 기준 상대 경로. 설정 파일에서 절대 경로로 덮어쓸 수 있다
DATA_PATHS = {
    "state_dir": "state",
    "run_dir": "run",
    # 예전 워커 구성 파일. 지금은 워커 설정이 워커 폴더에 있고(design.md 6.3.1), 이 파일은 처음 한 번 옮길 때만 읽는다
    "workers_file": "config/workers.json",
    "types_file": "config/worker-types.json",
    # 페이로드 type 조각. 시작 셸이 매번 다시 만든다 (애드온)
    "types_dir": "config/worker-types.d",
}

DEFAULTS = {
    "api": {
        "read_timeout_s": 1.0,
        "max_message_bytes": 65536,
        "subscribe_period_ms": 1000,
        "subscriber_timeout_s": 5.0,
    },
    "restart": {
        # 실패 시 계속 재시작(횟수 제한 없음, 멈추려면 외부에서 stop). 간격은 interval_s에서 연달아 실패할 때마다
        # 두 배씩 max_interval_s까지. healthy_s 넘게 돌았으면 처음으로. crash_window_s 안에 crash_count번 죽으면
        # 상태 crash_loop(화면 배지·기록). design.md 6.6.9 ②
        "interval_s": 2.0,
        "max_interval_s": 30.0,
        "healthy_s": 60.0,
        "crash_window_s": 300.0,
        "crash_count": 5,
    },
    "worker_defaults": {
        "start_timeout_sec": 10,
        "watchdog_sec": 3,
        "stop_timeout_sec": 5,
    },
    "log": {
        "max_bytes": 10 * 1024 * 1024,
        "backups": 3,
    },
    "tick_s": 0.2,
}



# 제품 모드(제품 빌드의 서비스 파일이 정한다, design.md 11.4). 개발 워크스페이스는 둘 다 꺼짐/허용
PRODUCT = os.environ.get("WORKSPACE_PRODUCT", "0") == "1"
# 설정 쓰기(설정 저장·임시 적용·프리셋 바꾸기·자동 시작). 제품 기본은 잠금(0)
CONFIG_WRITE = os.environ.get("WORKSPACE_CONFIG_WRITE", "0" if PRODUCT else "1") == "1"


def manager_api_workers(types):
    """Manager API를 쓰는 워커(워커 선언 "manager_api": true, design.md 6.5) → 워커 id 집합.
    Manager가 그 워커에게만 전용 소켓 manager-<워커 id>.sock을 준다. 플랫폼 클라이언트(rest, logger)와 이름이 겹치면 안 된다"""
    out = set()
    for tname, t in types.items():
        if t.get("manager_api") is not True:
            continue
        wid = t.get("_worker_id")
        if not wid or wid in ("rest", "logger"):
            log.error("worker type %r: manager_api ignored (worker id %r)", tname, wid)
            continue
        out.add(wid)
    return out


def _merge(base, override):
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(base.get(key), dict):
            _merge(base[key], value)
        else:
            base[key] = value
    return base


def load_settings(path, data_dir):
    """기본값 + 설정 파일. 파일이 없거나 깨졌으면 기본값으로 뜬다(경고만)."""
    settings = copy.deepcopy(DEFAULTS)
    if path:
        try:
            with open(path, encoding="utf-8") as f:
                override = json.load(f)
            if not isinstance(override, dict):
                raise ValueError("must be an object")
            _merge(settings, override)
        except FileNotFoundError:
            log.warning("settings %s not found, using defaults", path)
        except (OSError, ValueError) as e:
            log.error("settings %s unusable (%s), using defaults", path, e)
    for key, rel in DATA_PATHS.items():
        if settings.get(key) is None:  # 빈 문자열은 "사용 안 함"(예: types_file)
            if not data_dir:
                raise ValueError("%s not set and WORKSPACE_DATA not given" % key)
            settings[key] = os.path.join(data_dir, rel)
    return settings




def _read_type_files(path, directory):
    files = []
    if path and os.path.exists(path):
        files.append(path)
    if directory and os.path.isdir(directory):
        files.extend(os.path.join(directory, n) for n in sorted(os.listdir(directory))
                     if n.endswith(".json"))
    raw = {}
    for file in files:
        # 깨진 조각 하나 때문에 Manager가 뜨지 못하면 안 된다. 그 파일만 건너뛴다
        try:
            with open(file, encoding="utf-8") as f:
                entries = json.load(f)
            if not isinstance(entries, dict):
                raise ValueError("must be an object")
        except (OSError, ValueError) as e:
            log.error("worker types %s skipped: %s", file, e)
            continue
        # worker-types.d/<워커 폴더 이름>.json(prepare.sh가 만든 조각)이면 그 워커 폴더를 기억한다
        folder = os.path.basename(file)[:-5] if directory and os.path.dirname(file) == directory else None
        for name, entry in entries.items():
            if name in raw:
                log.warning("worker type %r redefined in %s", name, file)
            if isinstance(entry, dict) and folder:
                entry = dict(entry, _folder=folder)
            raw[name] = entry
    return raw


def _worker_conf_dir(root, folder):
    """워커 폴더의 설정 파일 자리(workspace/, 예전 워커는 폴더 맨 위). 폴더가 없으면 None"""
    w = os.path.join(root, "workers", folder)
    if not os.path.isdir(w):
        return None
    ws = os.path.join(w, "workspace")
    return ws if os.path.isdir(ws) else w


def load_types(path, directory, worker_defaults):
    """워커 type 레지스트리 (design.md 6.6). exec는 문자열 또는 argv 리스트.

    types_file과 types_dir/*.json을 합친다. 잘못된 type은 건너뛰고, 그 type의 워커는 error가 된다.
    """
    raw = _read_type_files(path, directory)
    if not raw:
        log.warning("no worker types registered")
    types = {}
    for name, entry in raw.items():
        if not isinstance(entry, dict):
            log.error("worker type %r skipped: must be an object", name)
            continue
        t = dict(worker_defaults)
        t.update(entry)
        if isinstance(t.get("exec"), str):
            t["exec"] = [t["exec"]]
        if not isinstance(t.get("exec_candidates"), list):
            t.pop("exec_candidates", None)  # 실행 파일 후보(띄울 때 첫째 있는 것, supervisor._spawn)
        if not t.get("exec"):
            log.error("worker type %r skipped: exec missing", name)
            continue
        t.pop("role", None)  # 예전 선언의 role(producer/consumer)은 쓰지 않는다. 워커끼리는 독립(design.md 6.6.6)
        folder = t.pop("_folder", None)
        conf_dir = _worker_conf_dir(WORKSPACE_ROOT, folder) if folder else None
        if conf_dir:
            # 워커 폴더 하나 = 워커 하나. id는 폴더 이름, 설정은 그 폴더의 config.json·presets/ (design.md 6.3.1)
            if any(x.get("_worker_id") == folder for x in types.values()):
                log.error("worker type %r skipped: worker folder %s already defines a type", name, folder)
                continue
            t["_worker_id"] = folder
            t["_conf_dir"] = conf_dir
        types[name] = t
    return types


# ---- 기본 탑재 워커 (design.md 6.9) ---------------------------------------------------------------
# 플랫폼 부품 가운데 워커 계약으로 도는 것(예: 시스템 모니터 monitor/)은 자기 폴더 맨 위에 worker-types.json을 둔다.
# Manager는 이름을 모른다. <워크스페이스>/*/worker-types.json을 읽고, type의 선언만 따른다:
#   "builtin":  {"id": "<워커 id>", "autostart": true, "config": {}}  → workers.json에 그 type의 워커가 없으면 등록
#   "requires": ["<도구>", ...]                                     → 하나라도 없으면 사용 안 함(enabled: false)으로 등록
# exec 안의 @WORKSPACE_ROOT@는 워크스페이스 경로로 바꾼다. 워커 폴더(workers/*)는 여기 해당하지 않는다(prepare.sh가 등록)
WORKSPACE_ROOT = os.path.abspath(os.environ.get("WORKSPACE_ROOT") or os.path.join(os.path.dirname(__file__), "..", ".."))


def load_builtin_types(worker_defaults, root=WORKSPACE_ROOT):
    """<root>/*/worker-types.json → {type: 정의}. 잘못된 항목은 건너뛴다"""
    import glob
    types = {}
    for path in sorted(glob.glob(os.path.join(root, "*", "worker-types.json"))):
        try:
            with open(path, encoding="utf-8") as f:
                entries = json.loads(f.read().replace("@WORKSPACE_ROOT@", root))
        except (OSError, ValueError) as e:
            log.error("built-in types %s skipped: %s", path, e)
            continue
        for name, entry in entries.items():
            if not isinstance(entry, dict) or not entry.get("exec"):
                log.error("built-in type %r in %s skipped: needs exec", name, path)
                continue
            t = dict(worker_defaults)
            t.update(entry)
            t.pop("role", None)
            b = t.get("builtin")
            if isinstance(b, dict) and isinstance(b.get("id"), str):
                t["_worker_id"] = b["id"]
                t["_conf_dir"] = os.path.dirname(path)  # 부품 폴더 맨 위에 config.json·presets/
            if isinstance(t["exec"], str):
                t["exec"] = [t["exec"]]
            types[name] = t
    return types


def add_builtin_types(types, worker_defaults, root=WORKSPACE_ROOT):
    """레지스트리에 기본 탑재 type을 더한다(데이터 폴더 레지스트리에 같은 이름이 있으면 그것을 쓴다)"""
    for name, t in load_builtin_types(worker_defaults, root).items():
        types.setdefault(name, t)
    return types


def builtin_workers(types):
    """type 선언의 "builtin" → {워커 id: spec}"""
    out = {}
    for name, t in types.items():
        b = t.get("builtin")
        if isinstance(b, dict) and isinstance(b.get("id"), str):
            out[b["id"]] = {"type": name, "autostart": b.get("autostart", True), "config": b.get("config", {})}
    return out


def requirements_met(t):
    """type 선언의 "requires" 도구가 모두 있으면 True"""
    import shutil
    return all(shutil.which(x) or os.path.exists(os.path.join("/usr/bin", x)) for x in t.get("requires", []))
