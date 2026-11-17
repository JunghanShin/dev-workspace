"""Manager 실행 기록. Manager만 쓴다 (design.md 6.3, 11장).

워커 설정은 여기 없다. 워커 폴더의 config.json·presets/에 있다(workerfiles.py, design.md 6.3.1).
여기 두는 것은 이 보드의 실행 기록뿐이다. 데이터 폴더가 지워져도 쓸 때마다 폴더를 다시 만든다.

run_dir/desired.json (<데이터>/run)
  현재 실행 상태(start/stop 명령 결과)와 그때의 boot_id. 같은 부팅이면 이 값으로 복원,
  boot_id가 다르면(재부팅) 무시하고 autostart를 따른다
run_dir/temp_config.json
  저장하지 않고 '적용'만 한 워커 설정(임시)과 boot_id. 재부팅하면 버린다

state_dir/ (<데이터>/state)
  versions.json       워커별 config_version과 그 버전의 config
  workers/{id}.json   spawn 시 워커에 넘기는 설정 파일
  logs/{id}.log       워커 stdout/stderr

workers_file (<데이터>/config/workers.json): 예전 워커 구성 파일. 있으면 처음 한 번 워커 폴더로 옮기고
workers.json.moved로 이름을 바꾼다.
"""
import json
import logging
import os
import shutil

log = logging.getLogger("workspace.manager")

def _fsync_dir(path):
    dir_fd = os.open(os.path.dirname(path) or ".", os.O_RDONLY)
    try:
        os.fsync(dir_fd)
    finally:
        os.close(dir_fd)


def boot_id():
    try:
        with open("/proc/sys/kernel/random/boot_id", encoding="ascii") as f:
            return f.read().strip()
    except OSError:
        return None


def _ensure_parent(path):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)


def atomic_write_text(path, text):
    """임시 파일 → fsync → rename. 전원 차단에도 반쯤 쓴 파일이 남지 않게."""
    _ensure_parent(path)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    _fsync_dir(path)


def atomic_write_json(path, obj):
    atomic_write_text(path, _dumps(obj))


def _dumps(obj):
    return json.dumps(obj, ensure_ascii=False, indent=2) + "\n"


def _read_text(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        return None


class Store:
    def __init__(self, state_dir, workers_file, run_dir):
        self.state_dir = state_dir
        self.workers_file = workers_file
        self.runtime_path = os.path.join(run_dir, "desired.json")
        self.temp_path = os.path.join(run_dir, "temp_config.json")
        self.versions_path = os.path.join(state_dir, "versions.json")
        self.workers_dir = os.path.join(state_dir, "workers")
        self.logs_dir = os.path.join(state_dir, "logs")
        for d in (state_dir, self.workers_dir, self.logs_dir):
            os.makedirs(d, exist_ok=True)

    # ---- 예전 workers.json (한 번 옮기기) ------------------------------

    def load_legacy(self):
        """{id: 항목} 또는 빈 dict"""
        text = _read_text(self.workers_file)
        if text is None:
            return {}
        try:
            data = json.loads(text)
        except ValueError as e:
            log.error("%s is not valid JSON (%s); not moved", self.workers_file, e)
            return {}
        return data if isinstance(data, dict) else {}

    def retire_legacy(self):
        try:
            os.replace(self.workers_file, self.workers_file + ".moved")
            log.info("%s moved to worker folders; renamed to workers.json.moved", self.workers_file)
        except OSError as e:
            log.error("cannot rename %s: %s", self.workers_file, e)

    # ---- 이번 부팅의 실행 상태·임시 설정 ---------------------------------

    def _load_boot_file(self, path):
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            return {}
        if not isinstance(data, dict) or data.get("boot_id") != boot_id():
            return {}
        return data["workers"] if isinstance(data.get("workers"), dict) else {}

    def load_runtime(self):
        """{id: "running"|"stopped"}. 다른 부팅에서 쓴 것이거나 없으면 빈 dict(autostart를 따름)"""
        return self._load_boot_file(self.runtime_path)

    def save_runtime(self, states):
        atomic_write_json(self.runtime_path, {"boot_id": boot_id(), "workers": states})

    def load_temp(self):
        """{id: config} 이번 부팅에서 저장하지 않고 적용한 설정"""
        return self._load_boot_file(self.temp_path)

    def save_temp(self, configs):
        atomic_write_json(self.temp_path, {"boot_id": boot_id(), "workers": configs})

    def load_versions(self):
        try:
            with open(self.versions_path, encoding="utf-8") as f:
                return json.load(f)
        except (FileNotFoundError, ValueError):
            return {}

    def save_versions(self, versions):
        atomic_write_json(self.versions_path, versions)

    # ---- 워커 실행 파일 --------------------------------------------------

    def write_worker_config(self, worker_id, payload):
        path = os.path.join(self.workers_dir, worker_id + ".json")
        atomic_write_json(path, payload)
        return path

    def prune_worker_configs(self, keep_ids):
        """지금 워커가 아닌 state/workers/<id>.json(빠진 워커, 예전 id)을 지운다. 로그(state/logs)는 기록이라 둔다"""
        try:
            names = os.listdir(self.workers_dir)
        except OSError:
            return
        for f in names:
            if f.endswith(".json") and f[:-5] not in keep_ids:
                try:
                    os.remove(os.path.join(self.workers_dir, f))
                    log.info("removed stale worker config %s", f)
                except OSError as e:
                    log.warning("cannot remove %s: %s", f, e)

    def remove_worker_config(self, worker_id):
        try:
            os.remove(os.path.join(self.workers_dir, worker_id + ".json"))
        except FileNotFoundError:
            pass

    def log_path(self, worker_id):
        os.makedirs(self.logs_dir, exist_ok=True)
        return os.path.join(self.logs_dir, worker_id + ".log")

    def rotate_logs(self, max_bytes, backups):
        """copy-truncate 방식. 워커는 O_APPEND로 쓰므로 truncate 후에도 끝에 이어 쓴다."""
        try:
            names = os.listdir(self.logs_dir)
        except FileNotFoundError:
            return
        for name in names:
            if not name.endswith(".log"):
                continue
            path = os.path.join(self.logs_dir, name)
            try:
                if os.path.getsize(path) <= max_bytes:
                    continue
                for i in range(backups - 1, 0, -1):
                    src = "%s.%d" % (path, i)
                    if os.path.exists(src):
                        os.replace(src, "%s.%d" % (path, i + 1))
                if backups > 0:
                    shutil.copyfile(path, path + ".1")
                os.truncate(path, 0)
            except OSError:
                continue
