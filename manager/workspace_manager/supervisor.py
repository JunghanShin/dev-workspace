"""워커 생명주기: 등록·기동·정지·감시·종료 판정·재기동 (design.md 6.3).

워커는 자기 일만 하고, 판단은 전부 여기서 한다.
"""
import asyncio
import ctypes
import logging
import os
import re
import signal
import time

from . import settings as settings_mod  # 제품 모드 스위치(PRODUCT)
from datetime import datetime, timezone

from .resources import ResourceSampler
from .schema import check_config, worker_refs
from .settings import requirements_met
from .workerfiles import DEFAULT_PRESET, WorkerFileError, WorkerFiles, check_name

log = logging.getLogger("workspace.manager")

ID_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")

EXIT_CONFIG = 1

# 정상 정지로 끝나는 종료 사유. 이 외(None, "stalled")는 실패로 판정한다.
_REQUESTED = ("requested", "restart", "shutdown")

# heartbeat 예약 키. 나머지 key=value는 워커 지표 (design.md 9.5)
_HB_KEYS = ("READY", "WATCHDOG", "progress", "config_version", "id", "libworker", "sim")

# 바로 적용(SIGHUP) 후 워커가 새 config_version을 heartbeat로 알려야 하는 시간. 넘으면 재시작
LIVE_ACK_S = 5.0


def _changed_paths(old, new):
    """바뀐 설정 경로. 모듈 칸(dict)은 한 단계 더 들어가 "<모듈>.<키>"(live_keys와 같은 형식)"""
    out = set()
    for k in set(old) | set(new):
        a, b = old.get(k), new.get(k)
        if a == b:
            continue
        if isinstance(a, dict) and isinstance(b, dict):
            out |= {"%s.%s" % (k, kk) for kk in set(a) | set(b) if a.get(kk) != b.get(kk)}
        else:
            out.add(k)
    return out


def _number(value):
    for kind in (int, float):
        try:
            return kind(value)
        except (TypeError, ValueError):
            pass
    return value

try:
    _libc = ctypes.CDLL("libc.so.6", use_errno=True)
except OSError:
    _libc = None


def _child_setup():
    # Manager가 systemd 밖에서 죽어도 워커가 고아로 남지 않게 (PR_SET_PDEATHSIG)
    if _libc is not None:
        _libc.prctl(1, signal.SIGTERM)


def _now_iso():
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


class ApiError(Exception):
    def __init__(self, code, message, fields=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.fields = fields  # 설정 검사 오류 {경로: 메시지} (schema.py)


class Worker:
    builtin_types = frozenset()  # 기본 탑재 type 이름(Supervisor가 type 선언에서 채운다)

    def __init__(self, record, files=None):
        self.rec = record
        self.files = files       # 워커 폴더의 설정 파일(config.json, presets/)
        self.file_error = False  # 설정 파일을 읽지 못함(그 워커만 error)
        self.libworker = None    # 워커가 READY로 알린 libworker 버전(계약 버전)
        self.sim = False         # 가짜 장치 모드로 돌고 있음(READY sim=1, design.md 6.6.8)
        self.peers = {}          # 설정이 가리키는 다른 워커(Supervisor가 설정이 바뀔 때 채움)
        self.exec_path = None    # 마지막으로 띄운 실행 파일(개발 빌드 build/ 인지 확정 workspace/bin/ 인지)
        self.proc = None
        self.actual = "stopped"
        self.last_exit_code = None
        self.last_signal = None
        self.last_error = None
        self.restart_count = 0  # 마지막 start 명령 이후 실패로 인한 재시작 횟수
        self.started_at = None
        self.spawn_mono = None
        self.last_hb_mono = None
        self.progress = None
        self.next_start_mono = 0.0
        self.applied_version = None
        self.live_version = None   # SIGHUP으로 보낸 설정 버전(워커 확인 대기 중)
        self.live_deadline = None
        self.stop_reason = None
        self.term_mono = None
        self.killed = False
        self.metrics = None    # 워커가 heartbeat로 보고한 지표 (design.md 9.5)
        self.resources = None  # Manager가 잰 프로세스 그룹 CPU·메모리

    @property
    def id(self):
        return self.rec["id"]

    def status(self):
        now = time.monotonic()
        hb_ago = None
        if self.proc is not None and self.last_hb_mono is not None:
            hb_ago = int((now - self.last_hb_mono) * 1000)
        return {
            "id": self.id,
            "type": self.rec["type"],
            "builtin": self.rec["type"] in Worker.builtin_types,  # 기본 탑재(type 선언 "builtin"). 화면은 워커 목록에서 뺀다
            "autostart": self.rec.get("autostart"),
            "preset": self.rec.get("preset"),   # 사용 중 프리셋(워커 폴더 presets/<이름>.json)
            "temp": self.rec.get("temp", False),  # 저장하지 않고 적용한 값으로 도는 중
            "libworker": self.libworker,
            "sim": self.sim,
            "exec": self.exec_path,
            "peers": self.peers,  # 설정이 가리키는 다른 워커 {설정 경로: 워커 id}(칸 형식 worker_id, design.md 6.6.7)
            "desired_state": self.rec["desired_state"],
            "actual_state": self.actual,
            "pid": self.proc.pid if self.proc is not None else None,
            "last_exit_code": self.last_exit_code,
            "last_signal": self.last_signal,
            "last_error": self.last_error,
            "restart_count": self.restart_count,
            "started_at": self.started_at,
            "last_heartbeat_ms_ago": hb_ago,
            "progress": self.progress,
            "config_version": self.rec["config_version"],
            "applied_version": self.applied_version,
            "metrics": self.metrics,
            "resources": self.resources,
        }


class Supervisor:
    def __init__(self, settings, types, store, notify_path):
        self.settings = settings
        self.types = types
        Worker.builtin_types = frozenset(n for n, t in types.items() if isinstance(t.get("builtin"), dict))
        self.store = store
        self.notify_path = notify_path
        self.workers = {}
        self.listeners = []
        self._wait_tasks = set()
        self.sampler = ResourceSampler()

    # ---- 영속화·이벤트 -------------------------------------------------

    def load(self):
        """워커 폴더를 보고 워커를 만든다. 워크스페이스는 워커 목록·설정을 들고 있지 않다 (design.md 6.3.1):
        워커 = 워커 폴더(type 선언에 _conf_dir), 설정 = 그 폴더의 config.json + presets/<active>.json.
        예전 <데이터>/config/workers.json이 있고 워커 폴더에 설정 파일이 없으면 그 값을 한 번 옮긴다."""
        legacy = self.store.load_legacy()
        versions = self.store.load_versions()
        # 같은 부팅에서 Manager만 재시작됐으면 직전 실행 상태·임시 설정, 부팅 직후면 비어 있어 autostart를 따른다
        runtime = self.store.load_runtime()
        temps = self.store.load_temp()
        log.info("run state: %s", "restored from %s" % self.store.runtime_path if runtime
                 else "boot (autostart)")
        for tname, t in sorted(self.types.items(), key=lambda kv: kv[1].get("_worker_id") or ""):
            worker_id = t.get("_worker_id")
            if not worker_id or not t.get("_conf_dir"):
                continue
            files = WorkerFiles(t["_conf_dir"])
            try:
                rec = self._load_files(worker_id, tname, t, files, legacy)
            except WorkerFileError as e:
                w = Worker({"id": worker_id, "type": tname, "autostart": False, "preset": None,
                            "saved": {}, "config": {}, "temp": False,
                            "desired_state": "stopped", "config_version": 0}, files)
                w.actual = "error"
                w.last_error = "설정 파일: %s" % e
                w.file_error = True
                log.error("%s: %s", worker_id, w.last_error)
                self.workers[worker_id] = w
                continue
            if worker_id in temps and isinstance(temps[worker_id], dict):
                rec["config"] = temps[worker_id]  # 이번 부팅에서 '적용'만 하고 저장하지 않은 값
                rec["temp"] = True
            errs = self._config_errors(tname, rec["config"])
            if errs:
                log.warning("%s: config does not match schema: %s", worker_id,
                            "; ".join("%s: %s" % kv for kv in sorted(errs.items())))
            prev = versions.get(worker_id)
            if prev is None:
                rec["config_version"] = 1
            elif prev.get("config") == rec["config"]:
                rec["config_version"] = prev.get("config_version", 1)
            else:
                rec["config_version"] = prev.get("config_version", 0) + 1
            rec["desired_state"] = runtime.get(worker_id) \
                if runtime.get(worker_id) in ("running", "stopped") \
                else ("running" if rec["autostart"] else "stopped")
            if not requirements_met(t):
                rec["desired_state"] = "stopped"
            w = Worker(rec, files)
            if not requirements_met(t):
                # 기본 탑재 워커의 필요 도구가 없는 기계(예: PC에는 tegrastats 없음): 띄우지 않는다
                w.actual = "disabled"
                w.last_error = "needs %s" % ", ".join(t.get("requires", []))
            w.peers = worker_refs(t.get("config_schema"), rec["config"])
            self.workers[worker_id] = w
        if legacy:
            self.store.retire_legacy()
        log.info("loaded %d worker(s) from worker folders", len(self.workers))
        self.store.prune_worker_configs(set(self.workers))  # 빠진 워커·예전 id의 실행 설정 파일 정리
        self._save()
        self._save_runtime()

    def _load_files(self, worker_id, tname, t, files, legacy):
        builtin = t.get("builtin") if isinstance(t.get("builtin"), dict) else {}
        cfg = files.read_config()
        if cfg is None or not files.has_preset(cfg["active"]):
            old = next((e for e in legacy.values() if isinstance(e, dict) and e.get("type") == tname), None)
            if old is not None and isinstance(old.get("config"), dict):
                # 예전 workers.json의 값을 워커 폴더로 옮긴다(한 번). 깃에 올리면 다른 보드도 같은 값
                autostart = old.get("autostart", old.get("desired_state") == "running")
                files.write_preset(DEFAULT_PRESET, "기본", old["config"])
                files.write_config(DEFAULT_PRESET, bool(autostart))
                log.info("%s: settings moved from workers.json to %s", worker_id, files.dir)
                cfg = files.read_config()
            elif cfg is None:
                cfg = {"active": DEFAULT_PRESET, "autostart": bool(builtin.get("autostart", False))}
        preset = files.read_preset(cfg["active"])
        if preset is None:
            # 프리셋 파일이 아직 없다: type 선언의 기본값(예전 "default"·"builtin"의 config)으로 돈다. 저장하면 파일이 생긴다
            d = t.get("default") if isinstance(t.get("default"), dict) else builtin
            preset = {"label": "기본", "config": dict(d.get("config") or {})}
        return {"id": worker_id, "type": tname, "autostart": cfg["autostart"], "preset": cfg["active"],
                "saved": preset["config"], "config": preset["config"], "temp": False}

    def _save_runtime(self):
        self.store.save_runtime({w.id: w.rec["desired_state"] for w in self.workers.values()
                                 if not w.file_error})
        self.store.save_temp({w.id: w.rec["config"] for w in self.workers.values() if w.rec.get("temp")})

    def _save(self):
        versions = {w.id: {"config_version": w.rec["config_version"], "config": w.rec["config"]}
                    for w in self.workers.values() if not w.file_error}
        self.store.save_versions(versions)

    def _emit(self, event, key, data):
        for listener in self.listeners:
            try:
                listener(event, key, data)
            except Exception:
                log.exception("listener failed")

    def _emit_worker(self, w):
        w.peers = worker_refs((self.types.get(w.rec.get("type")) or {}).get("config_schema"), w.rec.get("config"))
        self._emit("worker_status", w.id, w.status())

    # ---- 조회 ----------------------------------------------------------

    def _get(self, worker_id):
        w = self.workers.get(worker_id)
        if w is None:
            raise ApiError("not_found", "unknown worker id %r" % worker_id)
        return w

    def list(self):
        return [w.status() for w in self.workers.values()]

    def get(self, worker_id):
        return self._get(worker_id).status()

    def get_config(self, worker_id):
        """지금 값(config), 저장된 값(saved), 임시 여부, 사용 중인 프리셋과 프리셋 목록, 설정 파일 위치"""
        w = self._get(worker_id)
        out = {"id": w.id, "config": w.rec["config"], "saved": w.rec["saved"], "temp": w.rec["temp"],
               "preset": w.rec["preset"], "presets": w.files.presets() if w.files else [],
               "autostart": w.rec["autostart"], "config_version": w.rec["config_version"],
               "files": w.files.dir if w.files else None}
        errs = self._config_errors(w.rec.get("type"), w.rec["config"])
        if errs:
            out["config_errors"] = errs  # 파일을 직접 고쳐 스키마에 안 맞는 값(화면에 표시)
        return out

    def get_schema(self, worker_id):
        """화면이 설정 폼을 그리는 스키마(type 선언의 config_schema). 없으면 schema: null"""
        w = self._get(worker_id)
        t = self.types.get(w.rec.get("type")) or {}
        return {"id": w.id, "type": w.rec.get("type"), "schema": t.get("config_schema"),
                "live_keys": list(t.get("live_keys") or ())}

    def _config_errors(self, wtype, config):
        return check_config((self.types.get(wtype) or {}).get("config_schema"), config)

    def _check_schema(self, wtype, config):
        errs = self._config_errors(wtype, config)
        if errs:
            raise ApiError("bad_request", "설정 값이 맞지 않습니다: " + "; ".join(
                "%s: %s" % kv for kv in sorted(errs.items())[:5]), fields=errs)

    # ---- 명령 (요청 처리 중 워커 기동·종료를 기다리지 않는다) ----------

    def _check_device_conflict(self, worker_id, config):
        sn = config.get("device_sn")
        if sn is None:
            return
        for other in self.workers.values():
            other_cfg = other.rec.get("config")
            if other.id != worker_id and isinstance(other_cfg, dict) and other_cfg.get("device_sn") == sn:
                raise ApiError("conflict", "device_sn %r already used by %r" % (sn, other.id))

    def _get_valid(self, worker_id):
        w = self._get(worker_id)
        if w.file_error:
            raise ApiError("conflict", "%s; fix the file in the worker folder and restart the workspace"
                           % w.last_error)
        return w

    def _files_call(self, fn, *args):
        try:
            return fn(*args)
        except WorkerFileError as e:
            raise ApiError("bad_request", str(e))
        except OSError as e:
            raise ApiError("internal", "worker folder write failed: %s" % e)

    def _change_config(self, w, config, saved=None, temp=False):
        """워커의 지금 값을 바꾸고 적용한다(재시작, live_keys만이면 SIGHUP)"""
        old = w.rec["config"]
        w.rec["config"] = config
        w.rec["temp"] = temp
        if saved is not None:
            w.rec["saved"] = saved
        if config != old:
            w.rec["config_version"] += 1
            if w.proc is None:
                w.next_start_mono = 0.0  # 재시작 대기 중이면 새 설정으로 바로
            else:
                self._try_live(w, old)
        self._save()
        self._save_runtime()
        self._emit_worker(w)

    def set_config(self, worker_id, config, save=True):
        """save=True: 사용 중인 프리셋 파일에 저장(깃에 올라가는 파일). False: 이번 부팅 동안만 적용(임시)"""
        w = self._get_valid(worker_id)
        if not isinstance(config, dict):
            raise ApiError("bad_request", "config must be an object")
        self._check_schema(w.rec["type"], config)
        self._check_device_conflict(worker_id, config)
        if save:
            label = (self._files_call(w.files.read_preset, w.rec["preset"]) or {}).get("label") or "기본"
            self._files_call(w.files.write_preset, w.rec["preset"], label, config)
            if w.files.read_config() is None:
                self._files_call(w.files.write_config, w.rec["preset"], w.rec["autostart"])
            log.info("%s: saved to preset %s", w.id, w.rec["preset"])
            self._change_config(w, config, saved=config, temp=False)
        else:
            log.info("%s: applied (temporary, not saved)", w.id)
            self._change_config(w, config, temp=config != w.rec["saved"])
        return w.status()

    def use_preset(self, worker_id, name):
        """사용 중 프리셋을 바꾼다(config.json에 기록) → 그 값으로 다시 시작"""
        w = self._get_valid(worker_id)
        preset = self._files_call(w.files.read_preset, name)
        if preset is None:
            raise ApiError("not_found", "no preset %r" % name)
        self._check_device_conflict(worker_id, preset["config"])
        self._files_call(w.files.write_config, name, w.rec["autostart"])
        w.rec["preset"] = name
        log.info("%s: preset %s", w.id, name)
        self._change_config(w, preset["config"], saved=preset["config"], temp=False)
        return w.status()

    def save_preset(self, worker_id, spec):
        """새 이름으로 저장: spec {"name", "label"?, "config"?(없으면 지금 값), "use"?(기본 true)}"""
        w = self._get_valid(worker_id)
        if not isinstance(spec, dict):
            raise ApiError("bad_request", 'body must be {"name": ..., "label": ...}')
        name = spec.get("name")
        self._files_call(check_name, name)
        if w.files.has_preset(name):
            raise ApiError("conflict", "preset %r already exists" % name)
        config = spec.get("config", w.rec["config"])
        if not isinstance(config, dict):
            raise ApiError("bad_request", "config must be an object")
        self._check_schema(w.rec["type"], config)
        self._files_call(w.files.write_preset, name, spec.get("label") or name, config)
        log.info("%s: preset %s created", w.id, name)
        if spec.get("use", True):
            return self.use_preset(worker_id, name)
        self._emit_worker(w)
        return w.status()

    def rename_preset(self, worker_id, name, spec):
        """spec {"name"?: 새 파일 이름, "label"?: 화면 이름}"""
        w = self._get_valid(worker_id)
        preset = self._files_call(w.files.read_preset, name)
        if preset is None:
            raise ApiError("not_found", "no preset %r" % name)
        spec = spec if isinstance(spec, dict) else {}
        new = spec.get("name") or name
        self._files_call(check_name, new)
        if new != name:
            if w.files.has_preset(new):
                raise ApiError("conflict", "preset %r already exists" % new)
            self._files_call(w.files.rename_preset, name, new)
            if w.rec["preset"] == name:
                w.rec["preset"] = new
                self._files_call(w.files.write_config, new, w.rec["autostart"])
        if spec.get("label"):
            self._files_call(w.files.write_preset, new, spec["label"], preset["config"])
        self._emit_worker(w)
        return w.status()

    def delete_preset(self, worker_id, name):
        w = self._get_valid(worker_id)
        if name == w.rec["preset"]:
            raise ApiError("conflict", "preset %r is in use; switch to another preset first" % name)
        if not w.files.has_preset(name):
            raise ApiError("not_found", "no preset %r" % name)
        self._files_call(w.files.delete_preset, name)
        log.info("%s: preset %s deleted", w.id, name)
        self._emit_worker(w)
        return w.status()

    def _try_live(self, w, old):
        """바뀐 키가 전부 type의 live_keys 안이면 다시 시작하지 않고 설정 파일만 바꿔 SIGHUP.
        워커가 heartbeat config_version으로 확인하지 않으면 _check_alive가 재시작한다."""
        t = self.types.get(w.rec["type"])
        live = set((t or {}).get("live_keys") or ())
        new = w.rec["config"]
        changed = _changed_paths(old, new)
        in_sync = w.applied_version == w.rec["config_version"] - 1 or w.live_version is not None
        covered = all(p in live or p.split(".", 1)[0] in live for p in changed)
        if not live or not changed or not covered or w.actual != "running" \
                or w.stop_reason is not None or not in_sync:
            return
        self.store.write_worker_config(w.id, self._worker_payload(w, t))
        w.live_version = w.rec["config_version"]
        w.live_deadline = time.monotonic() + LIVE_ACK_S
        log.info("%s live update v%d (%s), SIGHUP", w.id, w.live_version, ", ".join(sorted(changed)))
        self._signal(w, signal.SIGHUP)

    def start(self, worker_id):
        w = self._get_valid(worker_id)
        if w.actual == "disabled":
            raise ApiError("conflict", "worker cannot run on this machine (%s)" % w.last_error)
        if w.rec["type"] not in self.types:
            raise ApiError("conflict", w.last_error or "unknown worker type")
        w.rec["desired_state"] = "running"
        if w.proc is None:
            # 재시작 대기 중이어도 명령이 오면 바로 띄운다
            w.restart_count = 0
            w.next_start_mono = 0.0
        self._save_runtime()  # 현재 실행 상태만. 부팅 정책(autostart)은 바꾸지 않는다
        self._emit_worker(w)
        return w.status()

    def stop(self, worker_id):
        w = self._get_valid(worker_id)
        w.rec["desired_state"] = "stopped"
        if w.proc is None and w.actual != "disabled":
            w.actual = "stopped"
        self._save_runtime()
        self._emit_worker(w)
        return w.status()

    def set_autostart(self, worker_id, value):
        """부팅 정책(워커 폴더 config.json). 지금 실행 상태는 바꾸지 않는다 (design.md 6.3.1)"""
        w = self._get_valid(worker_id)
        if isinstance(value, dict):  # REST 본문 {"autostart": true}
            value = value.get("autostart")
        if not isinstance(value, bool):
            raise ApiError("bad_request", "autostart must be true or false")
        self._files_call(w.files.write_config, w.rec["preset"], value)
        w.rec["autostart"] = value
        self._emit_worker(w)
        return w.status()

    # ---- heartbeat (design.md 9.5) -------------------------------------

    def on_notify(self, pid, fields):
        w = next((x for x in self.workers.values()
                  if x.proc is not None and x.proc.pid == pid), None)
        if w is None or w.stop_reason is not None:
            return
        now = time.monotonic()
        if "READY" in fields and w.actual == "starting":
            w.libworker = fields.get("libworker")
            w.sim = fields.get("sim") == "1"
            w.actual = "running"
            w.last_hb_mono = now
            log.info("%s ready (pid %d)", w.id, pid)
            self._emit_worker(w)
        if "WATCHDOG" in fields:
            w.last_hb_mono = now
            if "progress" in fields:
                try:
                    progress = int(fields["progress"])
                except ValueError:
                    return
                w.progress = progress
            version = _number(fields.get("config_version"))
            if w.live_version is not None and isinstance(version, int) and version >= w.live_version:
                log.info("%s applied config v%d live", w.id, version)
                w.applied_version = version
                if version >= w.rec["config_version"]:
                    w.live_version = w.live_deadline = None
                self._emit_worker(w)
            metrics = {k: _number(v) for k, v in fields.items() if k not in _HB_KEYS}
            if metrics:
                w.metrics = metrics
                self._emit_worker(w)  # 구독자(로거·UI)는 1초마다 지표를 받는다

    # ---- 루프 -----------------------------------------------------------

    async def run(self):
        tick = self.settings["tick_s"]
        last_rotate = last_sample = 0.0
        while True:
            await self._tick()
            now = time.monotonic()
            if now - last_sample >= 1.0:
                last_sample = now
                self._sample_resources()
            if now - last_rotate >= 10.0:
                last_rotate = now
                self.store.rotate_logs(self.settings["log"]["max_bytes"],
                                       self.settings["log"]["backups"])
            await asyncio.sleep(tick)

    def _sample_resources(self):
        running = {w.proc.pid: w for w in self.workers.values() if w.proc is not None}
        for pgid, usage in self.sampler.sample(list(running)).items():
            running[pgid].resources = usage

    async def _tick(self):
        now = time.monotonic()
        for w in list(self.workers.values()):
            t = self.types.get(w.rec["type"])
            if w.proc is not None:
                self._check_alive(w, t, now)
            elif t is not None:
                await self._check_idle(w, t, now)

    def _check_alive(self, w, t, now):
        if w.stop_reason is not None:
            if not w.killed and now - w.term_mono >= t["stop_timeout_sec"]:
                log.warning("%s did not stop in %ss, SIGKILL", w.id, t["stop_timeout_sec"])
                self._signal(w, signal.SIGKILL)
                w.killed = True
            return
        if w.rec["desired_state"] == "stopped":
            self._terminate(w, "requested")
            return
        if w.applied_version != w.rec["config_version"]:
            if w.live_version == w.rec["config_version"] and now < w.live_deadline:
                return  # 바로 적용 확인 대기
            if w.live_version is not None:
                log.warning("%s live update v%d not applied in %ss, restarting",
                            w.id, w.live_version, LIVE_ACK_S)
            w.live_version = w.live_deadline = None
            self._terminate(w, "restart")
            return
        if w.actual == "starting":
            if now - w.spawn_mono >= t["start_timeout_sec"]:
                w.last_error = "stalled: no READY in %ss" % t["start_timeout_sec"]
                self._terminate(w, "stalled")
            return
        # Manager는 워커 프로세스만 본다(heartbeat). progress는 장비가 끊기면 정상 워커도
        # 늘지 않으므로 멈춤 판단에 쓰지 않는다(design.md 9장). 장비 상태는 워커가 지표로 보고한다.
        watchdog = t["watchdog_sec"]
        if now - w.last_hb_mono >= watchdog:
            w.last_error = "stalled: no heartbeat for %ss" % watchdog
            self._terminate(w, "stalled")

    async def _check_idle(self, w, t, now):
        if w.rec["desired_state"] != "running" or w.actual in ("error", "disabled"):
            return
        if now < w.next_start_mono:
            return
        if settings_mod.PRODUCT and isinstance(w.rec.get("config"), dict) and "sim" in w.rec["config"]:
            # 제품에서는 가짜 장치 모드로 띄우지 않는다(실기로 착각할 위험)
            w.actual = "error"
            w.last_error = "sim (fake device) not allowed in product"
            log.error("%s: %s", w.id, w.last_error)
            return
        await self._spawn(w, t)

    def _worker_payload(self, w, t):
        payload = {
            "id": w.id,
            "type": w.rec["type"],
            "config_version": w.rec["config_version"],
            "config": w.rec["config"],
        }
        return payload

    async def _spawn(self, w, t):
        now = time.monotonic()
        cfg_path = self.store.write_worker_config(w.id, self._worker_payload(w, t))
        argv = list(t["exec"]) + ["--config", cfg_path]
        # 실행 파일 후보(type "exec_candidates", prepare.sh가 적음): 띄울 때마다 있는 것 중 첫째를 고른다.
        # 개발 빌드를 build/에 올리고 그 워커만 다시 시작하면 반영된다(워크스페이스 재시작 없이, dev/deploy-worker.sh).
        # 제품 모드에서는 쓰지 않는다(확정 실행 파일 exec만)
        for c in () if settings_mod.PRODUCT else (t.get("exec_candidates") or ()):
            if isinstance(c, str) and os.access(c, os.X_OK):
                argv[0] = c
                break
        w.exec_path = argv[0]
        env = dict(os.environ)
        env["WORKSPACE_WORKER_ID"] = w.id
        env["WORKSPACE_NOTIFY_SOCKET"] = self.notify_path
        # Manager API를 쓰는 워커(워커 선언 "manager_api": true)에게만 전용 Manager 소켓을 알려 준다(design.md 6.5)
        env.pop("WORKSPACE_MANAGER_SOCKET", None)
        if w.id in getattr(self, "manager_api_workers", ()):
            env["WORKSPACE_MANAGER_SOCKET"] = os.path.join(self.settings["run_dir"], "manager-%s.sock" % w.id)
        # 파이프를 쓰지 않는다: 파이프가 차면 워커가 블로킹된다 (design.md 9.4)
        logf = open(self.store.log_path(w.id), "ab")
        try:
            proc = await asyncio.create_subprocess_exec(
                *argv, stdin=asyncio.subprocess.DEVNULL, stdout=logf, stderr=logf,
                env=env, start_new_session=True, preexec_fn=_child_setup)
        except OSError as e:
            logf.close()
            w.last_error = "spawn failed: %s" % e
            log.error("%s spawn failed: %s", w.id, e)
            self._on_failure(w, now)
            self._emit_worker(w)
            return
        logf.close()
        w.proc = proc
        w.actual = "starting"
        w.started_at = _now_iso()
        w.spawn_mono = now
        w.last_hb_mono = None
        w.progress = None
        w.metrics = None
        w.resources = None
        w.applied_version = w.rec["config_version"]
        w.live_version = w.live_deadline = None
        w.stop_reason = None
        w.killed = False
        log.info("%s spawned pid %d (config v%d)", w.id, proc.pid, w.applied_version)
        self._emit_worker(w)
        task = asyncio.ensure_future(self._wait(w, proc))
        self._wait_tasks.add(task)
        task.add_done_callback(self._wait_tasks.discard)

    def _signal(self, w, sig):
        # SIGTERM은 워커에게만(워커가 자식 정리), SIGKILL은 프로세스 그룹 전체에
        try:
            if sig == signal.SIGKILL:
                os.killpg(w.proc.pid, sig)
            else:
                w.proc.send_signal(sig)
        except ProcessLookupError:
            pass

    @staticmethod
    def _reap_group(pid):
        """워커 종료 후 남은 자식(ffmpeg 등)을 정리. 워커는 자기 세션의 그룹 리더다."""
        try:
            os.killpg(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass

    def _terminate(self, w, reason):
        w.stop_reason = reason
        w.term_mono = time.monotonic()
        w.killed = False
        log.info("%s SIGTERM (%s)", w.id, reason)
        self._signal(w, signal.SIGTERM)
        self._emit_worker(w)

    async def _wait(self, w, proc):
        rc = await proc.wait()
        if w.proc is proc:
            self._on_exit(w, rc)

    def _on_exit(self, w, rc):
        now = time.monotonic()
        reason = w.stop_reason
        self._reap_group(w.proc.pid)
        w.proc = None
        w.metrics = None  # 지난 실행의 값이 현재 값처럼 보이지 않게
        w.resources = None
        w.stop_reason = None
        if rc < 0:
            w.last_exit_code = None
            try:
                w.last_signal = signal.Signals(-rc).name
            except ValueError:
                w.last_signal = str(-rc)
        else:
            w.last_exit_code = rc
            w.last_signal = None
        log.info("%s exited rc=%s reason=%s", w.id, rc, reason)

        if reason in _REQUESTED or (reason is None and w.rec["desired_state"] == "stopped"):
            w.actual = "stopped"
            w.next_start_mono = 0.0
            self._emit_worker(w)
            return

        # 이하 실패 (design.md 6.3 종료 판정 표). 사유만 기록하고 같은 간격으로 계속 재시작한다.
        # 계속할지 멈출지는 외부(REST·VSM)의 start/stop이 정한다
        if reason == "stalled":
            pass  # last_error는 판정 시점에 기록됨
        elif rc == 0:
            w.last_error = "unexpected exit 0"
        elif rc == EXIT_CONFIG:
            w.last_error = "config error (exit 1)"
        elif rc < 0:
            w.last_error = "killed by %s" % w.last_signal
        else:
            w.last_error = "exit %d" % rc
        self._on_failure(w, now)
        self._emit_worker(w)

    def _on_failure(self, w, now):
        interval = self.settings["restart"]["interval_s"]
        w.restart_count += 1
        w.actual = "exited"
        w.next_start_mono = now + interval
        log.warning("%s failed (%s), restart #%d in %.1fs",
                    w.id, w.last_error, w.restart_count, interval)

    async def shutdown(self):
        """Manager 종료 시 워커 정리. desired_state는 그대로 두어 재시작 시 복원."""
        running = [w for w in self.workers.values() if w.proc is not None]
        for w in running:
            if w.stop_reason is None:
                self._terminate(w, "shutdown")
        deadline = max((self.types.get(w.rec["type"], {}).get("stop_timeout_sec", 5)
                        for w in running), default=0)
        if self._wait_tasks:
            await asyncio.wait(list(self._wait_tasks), timeout=deadline)
        for w in running:
            if w.proc is not None:
                self._signal(w, signal.SIGKILL)
        if self._wait_tasks:
            await asyncio.wait(list(self._wait_tasks), timeout=2)
