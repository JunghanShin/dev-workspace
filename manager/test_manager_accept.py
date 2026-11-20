"""Manager 수락 시험: Unix 소켓 API만 보고 확인한다(검은 상자). Linux(PC WSL·보드)에서. 표준 라이브러리만.

    cd manager && python3 -m unittest test_manager_accept -v                                   # Python 판
    MANAGER_BIN=~/pm-build/process-manager python3 -m unittest test_manager_accept -v          # C++ 판

Python 판과 C++ process-manager가 둘 다 통과해야 바꿔 낀다(docs/process-manager.md 5장).
시험마다 임시 워크스페이스(워커 폴더)·데이터 폴더를 만들고 진짜 Manager를 띄운다. 워커는 아래 W_PY 하나를
설정 "behavior"로 다르게 움직인다(스키마에 없는 키라 검사하지 않음). 워커는 일어난 일을 out 파일에 JSON 한 줄씩 남긴다.
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))

# 시험 워커. 인자: <out 파일> --config <설정 파일>
W_PY = r'''
import json, os, signal, socket, subprocess, sys, time
out_path = sys.argv[1]
cfg_path = sys.argv[sys.argv.index("--config") + 1]

def load():
    with open(cfg_path, encoding="utf-8") as f:
        return json.load(f)

def log(**kv):
    with open(out_path, "a") as f:
        f.write(json.dumps(kv) + "\n")

d = load()
cfg = d.get("config", {})
b = cfg.get("behavior", {})
ver = d.get("config_version")
log(ev="start", pid=os.getpid(), pgid=os.getpgid(0), ver=ver, config=cfg, argv=sys.argv, payload=d,
    nice=os.nice(0), env={k: os.environ.get(k) for k in ("WORKSPACE_WORKER_ID", "WORKSPACE_NOTIFY_SOCKET",
                                                          "WORKSPACE_MANAGER_SOCKET", "WORKSPACE_DATA")})
print("hello stdout from %s" % d.get("id"), flush=True)
if b.get("log_bytes"):
    sys.stdout.write("x" * b["log_bytes"] + "\n")
    sys.stdout.flush()
sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
path = os.environ.get("WORKSPACE_NOTIFY_SOCKET")

def send(text):
    try:
        sock.sendto(text.encode(), path)
    except OSError:
        pass

state = {"stop": False, "hup": False}
def on_term(s, f): state["stop"] = True
def on_hup(s, f): state["hup"] = True
signal.signal(signal.SIGTERM, signal.SIG_IGN if b.get("term") == "ignore" else on_term)
signal.signal(signal.SIGHUP, on_hup)
if b.get("child"):
    # 같은 프로세스 그룹에 남는 자식(SIGTERM 무시). 그룹 SIGKILL로만 정리된다
    child = subprocess.Popen([sys.executable, "-c",
                              "import signal,time\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\nwhile True: time.sleep(1)"])
    log(ev="child", pid=child.pid)
if b.get("ready", True):
    send("READY=1 id=%s libworker=test-1%s" % (d.get("id"), " sim=1" if b.get("sim") else ""))
if b.get("exit") is not None:
    time.sleep(b.get("exit_after", 0.3))
    if b.get("kill_self"):
        os.kill(os.getpid(), getattr(signal, b["kill_self"]))
        time.sleep(5)
    sys.exit(b["exit"])
acked = ver
progress = 0
metrics = b.get("metrics") or {}
while not state["stop"]:
    if state["hup"]:
        state["hup"] = False
        nd = load()
        log(ev="hup", ver=nd.get("config_version"), config=nd.get("config"))
        if b.get("hup", "ack") == "ack":
            acked = nd.get("config_version")
    if b.get("heartbeat", True) and b.get("ready", True):
        progress += 1
        parts = ["WATCHDOG=1", "id=%s" % d.get("id"), "progress=%d" % progress, "config_version=%s" % acked]
        parts += ["%s=%s" % kv for kv in metrics.items()]
        send(" ".join(parts))
    time.sleep(0.2)
log(ev="stop", pid=os.getpid())
sys.exit(0)
'''

# 상태(8.2) 칸. 두 판이 같은 칸을 내야 한다
STATUS_KEYS = {"id", "type", "builtin", "autostart", "preset", "temp", "libworker", "sim", "exec", "peers",
               "desired_state", "actual_state", "pid", "last_exit_code", "last_signal", "last_error",
               "restart_count", "crash_loop", "restart_in_s", "process", "started_at", "last_heartbeat_ms_ago",
               "progress", "config_version", "applied_version", "metrics", "resources"}

SCHEMA = {"groups": [
    {"title": "모듈", "section": "mod", "fields": [
        {"key": "level", "type": "int", "min": 0, "max": 10, "default": 1},
        {"key": "name", "type": "string"},
        {"key": "peer", "type": "string", "format": "worker_id"}]},
    {"title": "호스트", "fields": [{"key": "watchdog_sec", "type": "number", "min": 1}]}]}

FAST_RESTART = {"restart": {"interval_s": 0.3, "max_interval_s": 1.0, "crash_count": 3, "crash_window_s": 60}}


def manager_cmd():
    b = os.environ.get("MANAGER_BIN")
    return [b] if b else [sys.executable, "-m", "workspace_manager"]


def wait_for(fn, timeout=15.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return None


def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False)


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def pid_alive(pid):
    try:
        with open("/proc/%d/stat" % pid) as f:
            return f.read().rsplit(")", 1)[1].split()[0] != "Z"
    except OSError:
        return False


class Env:
    """임시 워크스페이스 + 데이터 폴더 + Manager 하나.

    workers: {이름: spec}. spec 키(모두 선택):
      decl      type 선언에 더할 것(기본: exec, start_timeout_sec 5, watchdog_sec 3, stop_timeout_sec 2)
      config    presets/default.json의 config(None이면 프리셋 파일 없음)
      autostart config.json의 autostart(None이면 config.json 없음)
      raw_config_json  config.json에 그대로 쓸 글(깨진 파일 시험)
    """

    def __init__(self, workers, settings=None, product=False, builtins=None, legacy=None):
        self.tmp = tempfile.mkdtemp(prefix="ws-accept-")
        self.root = os.path.join(self.tmp, "ws")
        self.data = os.path.join(self.tmp, "data")
        self.run_dir = os.path.join(self.data, "run")
        self.product = product
        self.out = {}
        self.ws = {}
        for name, spec in workers.items():
            ws = os.path.join(self.root, "workers", name, "workspace")
            os.makedirs(ws)
            self.ws[name] = ws
            self.out[name] = os.path.join(self.tmp, name + ".out")
            with open(os.path.join(ws, "w.py"), "w") as f:
                f.write(W_PY)
            if spec.get("raw_config_json") is not None:
                with open(os.path.join(ws, "config.json"), "w") as f:
                    f.write(spec["raw_config_json"])
            elif spec.get("autostart") is not None:
                write_json(os.path.join(ws, "config.json"), {"active": "default", "autostart": spec["autostart"]})
            if spec.get("config") is not None:
                write_json(os.path.join(ws, "presets", "default.json"), {"label": "기본", "config": spec["config"]})
            decl = {"exec": [sys.executable, os.path.join(ws, "w.py"), self.out[name]],
                    "start_timeout_sec": 5, "watchdog_sec": 3, "stop_timeout_sec": 2}
            decl.update(spec.get("decl") or {})
            write_json(os.path.join(self.data, "config", "worker-types.d", name + ".json"), {name + "_t": decl})
        for part, (tname, decl) in (builtins or {}).items():
            # 기본 탑재 부품: <워크스페이스>/<부품>/worker-types.json, exec 안의 @WORKSPACE_ROOT@는 바뀐다
            d = os.path.join(self.root, part)
            os.makedirs(d)
            with open(os.path.join(d, "w.py"), "w") as f:
                f.write(W_PY)
            self.out[part] = os.path.join(self.tmp, part + ".out")
            decl = dict(decl)
            decl.setdefault("exec", [sys.executable, "@WORKSPACE_ROOT@/%s/w.py" % part, self.out[part]])
            write_json(os.path.join(d, "worker-types.json"), {tname: decl})
        if legacy is not None:
            write_json(os.path.join(self.data, "config", "workers.json"), legacy)
        mj = json.loads(json.dumps(FAST_RESTART))
        for k, v in (settings or {}).items():
            if isinstance(v, dict) and isinstance(mj.get(k), dict):
                mj[k].update(v)
            else:
                mj[k] = v
        write_json(os.path.join(self.data, "config", "manager.json"), mj)
        self.mgr = None

    def start(self):
        env = dict(os.environ, WORKSPACE_ROOT=self.root, WORKSPACE_DATA=self.data, PYTHONPATH=HERE,
                   PYTHONDONTWRITEBYTECODE="1", WORKSPACE_PRODUCT="1" if self.product else "0")
        env.pop("WORKSPACE_CONFIG_WRITE", None)
        self.log = open(os.path.join(self.tmp, "manager.log"), "ab")
        self.mgr = subprocess.Popen(manager_cmd() + ["--data", self.data], env=env,
                                    stdout=self.log, stderr=self.log)
        ok = wait_for(lambda: self._ping(), 10)
        assert ok, "manager did not come up: " + self.manager_log()
        return self

    def _ping(self):
        try:
            return self.call("manager.sock", {"op": "ping"}).get("ok")
        except OSError:
            return False

    def stop(self):
        if self.mgr is None:
            return
        self.mgr.terminate()
        try:
            self.mgr.wait(15)
        except subprocess.TimeoutExpired:
            self.mgr.kill()
            self.mgr.wait()
        self.mgr = None
        self.log.close()

    def close(self):
        self.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def manager_log(self):
        try:
            with open(os.path.join(self.tmp, "manager.log"), errors="replace") as f:
                return f.read()[-3000:]
        except OSError:
            return ""

    def sock(self, name):
        return os.path.join(self.run_dir, name)

    def raw(self, name, data, timeout=3.0):
        """보낸 바이트 그대로, 받은 바이트 그대로(닫힐 때까지)"""
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(timeout)
            s.connect(self.sock(name))
            if data:
                s.sendall(data)
            buf = b""
            while True:
                try:
                    d = s.recv(65536)
                except ConnectionResetError:
                    break
                if not d:
                    break
                buf += d
        return buf

    def call(self, name, req):
        data = self.raw(name, (json.dumps(req, ensure_ascii=False) + "\n").encode())
        return json.loads(data)

    def rest(self, req):
        r = self.call("manager-rest.sock", req)
        return r

    def ok(self, req):
        r = self.rest(req)
        assert r.get("ok"), (req, r)
        return r["data"]

    def status(self, wid=None):
        st = {w["id"]: w for w in self.call("manager.sock", {"op": "list"})["data"]}
        return st if wid is None else st.get(wid)

    def wait_state(self, wid, actual, timeout=15.0, **extra):
        def check():
            s = self.status(wid)
            if s and s["actual_state"] == actual and all(s.get(k) == v for k, v in extra.items()):
                return s
            return None
        s = wait_for(check, timeout)
        assert s, "%s never %s %s: %s\n%s" % (wid, actual, extra, self.status(wid), self.manager_log())
        return s

    def events(self, name):
        try:
            with open(self.out[name]) as f:
                return [json.loads(line) for line in f if line.strip()]
        except OSError:
            return []

    def starts(self, name):
        return [e for e in self.events(name) if e["ev"] == "start"]


class Subscriber:
    """구독 연결 하나. 받은 줄을 스레드가 읽어 쌓는다(시각과 함께)"""

    def __init__(self, path, req, read=True):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.s.sendall((json.dumps(req) + "\n").encode())
        self.lines = []
        self.closed = False
        self.lock = threading.Lock()
        if read:
            self.t = threading.Thread(target=self._run, daemon=True)
            self.t.start()

    def _run(self):
        buf = b""
        self.s.settimeout(0.5)
        while True:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                d = b""
            if not d:
                self.closed = True
                return
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                with self.lock:
                    self.lines.append((time.time(), json.loads(line)))

    def snapshot(self):
        with self.lock:
            return list(self.lines)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


@unittest.skipUnless(sys.platform.startswith("linux"), "Unix sockets, Linux only")
class Accept(unittest.TestCase):
    def setUp(self):
        self.envs = []

    def tearDown(self):
        for e in self.envs:
            e.close()

    def env(self, workers, **kw):
        e = Env(workers, **kw)
        self.envs.append(e)
        return e

    # ---- 조회·모양 -------------------------------------------------------

    def test_read_shapes(self):
        e = self.env({"w": {"config": {"mod": {"level": 2}}, "autostart": False,
                            "decl": {"config_schema": SCHEMA, "live_keys": ["mod.level"]}},
                      "bad": {"config": {"mod": {"level": 99}}, "autostart": False,
                              "decl": {"config_schema": SCHEMA}},
                      "plain": {"autostart": False}}).start()
        r = e.call("manager.sock", {"op": "ping"})
        self.assertEqual(r, {"ok": True, "data": {"pong": True}})
        st = e.status()
        self.assertEqual(set(st), {"w", "bad", "plain"})
        for wid, s in st.items():
            self.assertEqual(set(s), STATUS_KEYS, wid)
        w = st["w"]
        self.assertEqual((w["type"], w["builtin"], w["autostart"], w["preset"], w["temp"]), ("w_t", False, False, "default", False))
        self.assertEqual((w["desired_state"], w["actual_state"], w["pid"], w["config_version"]), ("stopped", "stopped", None, 1))
        self.assertEqual(st["plain"]["preset"], "default")
        # get
        self.assertEqual(e.call("manager.sock", {"op": "get", "id": "w"})["data"]["id"], "w")
        r = e.call("manager.sock", {"op": "get", "id": "nope"})
        self.assertEqual(r["error"]["code"], "not_found")
        self.assertIn("nope", r["error"]["message"])
        self.assertEqual(e.call("manager.sock", {"op": "get"})["error"]["code"], "not_found")
        # get_schema
        d = e.call("manager.sock", {"op": "get_schema", "id": "w"})["data"]
        self.assertEqual(d, {"id": "w", "type": "w_t", "schema": SCHEMA, "live_keys": ["mod.level"]})
        d = e.call("manager.sock", {"op": "get_schema", "id": "plain"})["data"]
        self.assertEqual(d, {"id": "plain", "type": "plain_t", "schema": None, "live_keys": []})
        # get_config
        d = e.call("manager.sock", {"op": "get_config", "id": "w"})["data"]
        self.assertEqual(set(d), {"id", "config", "saved", "temp", "preset", "presets", "autostart", "config_version", "files"})
        self.assertEqual(d["config"], {"mod": {"level": 2}})
        self.assertEqual(d["saved"], {"mod": {"level": 2}})
        self.assertEqual(d["presets"], [{"name": "default", "label": "기본"}])
        self.assertEqual(d["files"], e.ws["w"])
        d = e.call("manager.sock", {"op": "get_config", "id": "bad"})["data"]
        self.assertEqual(set(d["config_errors"]), {"mod.level"})
        d = e.call("manager.sock", {"op": "get_config", "id": "plain"})["data"]
        self.assertEqual((d["config"], d["presets"]), ({}, []))
        # system
        d = e.call("manager.sock", {"op": "system"})["data"]
        self.assertEqual(set(d), {"clients", "manager_api_workers", "product", "config_write"})
        self.assertEqual(set(d["clients"]), {"rest", "logger"})
        self.assertEqual((d["manager_api_workers"], d["product"], d["config_write"]), ([], False, True))
        self.assertIn(d["clients"]["rest"], ("idle", "disconnected"))
        # 데이터 폴더: 워커에 넘기는 설정 파일은 띄울 때 생긴다, 실행 기록 파일 모양
        v = read_json(os.path.join(e.data, "state", "versions.json"))
        self.assertEqual(v["w"], {"config_version": 1, "config": {"mod": {"level": 2}}})
        d = read_json(os.path.join(e.run_dir, "desired.json"))
        self.assertEqual(d["workers"], {"w": "stopped", "bad": "stopped", "plain": "stopped"})
        self.assertTrue(d["boot_id"])
        self.assertEqual(read_json(os.path.join(e.run_dir, "temp_config.json"))["workers"], {})
        mode = os.stat(e.sock("manager-rest.sock")).st_mode & 0o777
        self.assertEqual(mode, 0o600)

    def test_bad_requests(self):
        e = self.env({"w": {"config": {}, "autostart": False}}).start()
        for data in (b"not json\n", b"[1, 2]\n", b'{"id": "w"}\n', b'{"op": 5}\n'):
            r = json.loads(e.raw("manager-rest.sock", data))
            self.assertEqual(r["ok"], False, data)
            self.assertEqual(r["error"]["code"], "bad_request", data)
        r = e.call("manager-rest.sock", {"op": "nope"})
        self.assertEqual(r["error"], {"code": "bad_request", "message": "unknown op 'nope'"})
        # 줄바꿈 없이 끝(보내는 쪽이 닫음)도 한 건
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(3)
            s.connect(e.sock("manager.sock"))
            s.sendall(b'{"op": "ping"}')
            s.shutdown(socket.SHUT_WR)
            self.assertEqual(json.loads(s.recv(65536)), {"ok": True, "data": {"pong": True}})
        # 크기 상한(64 KiB) 넘는 요청: 답 없이 닫음
        big = b'{"op": "ping", "pad": "' + b"x" * 70000 + b'"}\n'
        try:
            self.assertEqual(e.raw("manager.sock", big), b"")
        except (BrokenPipeError, ConnectionResetError):
            pass
        # 아무것도 안 보내는 클라이언트: 1 s 뒤 닫음
        t0 = time.time()
        self.assertEqual(e.raw("manager.sock", b"", timeout=5), b"")
        self.assertLess(time.time() - t0, 3.0)
        # 그 뒤에도 정상
        self.assertTrue(e.call("manager.sock", {"op": "ping"})["ok"])
        # 읽기 전용 소켓은 모든 쓰기 op 거부
        for op in ("set_config", "use_preset", "save_preset", "rename_preset", "delete_preset",
                   "set_autostart", "start", "stop"):
            for sock in ("manager.sock", "manager-logger.sock"):
                r = e.call(sock, {"op": op, "id": "w"})
                self.assertEqual(r["error"]["code"], "forbidden", (op, sock))
        r = e.call("manager-logger.sock", {"op": "start", "id": "w"})
        self.assertEqual(r["error"]["message"], "logger is read-only")
        r = e.call("manager.sock", {"op": "start", "id": "w"})
        self.assertEqual(r["error"]["message"], "manager.sock is read-only")
        # 없는 워커
        for op in ("start", "stop", "set_config", "use_preset", "get_config", "get_schema", "get_io"):
            r = e.rest({"op": op, "id": "ghost", "config": {}, "name": "default"})
            self.assertEqual(r["error"]["code"], "not_found", op)
            self.assertEqual(r["error"]["message"], "unknown worker id 'ghost'", op)

    # ---- 설정 -------------------------------------------------------------

    def test_set_config_save_and_temp(self):
        base = {"mod": {"level": 1, "name": "a"}}
        e = self.env({"w": {"config": base, "autostart": True, "decl": {"config_schema": SCHEMA}}}).start()
        s1 = e.wait_state("w", "running")
        self.assertEqual((s1["config_version"], s1["applied_version"]), (1, 1))
        payload = read_json(os.path.join(e.data, "state", "workers", "w.json"))
        self.assertEqual(payload, {"id": "w", "type": "w_t", "config_version": 1, "config": base})
        self.assertEqual(e.starts("w")[-1]["argv"][-2:], ["--config", os.path.join(e.data, "state", "workers", "w.json")])
        env = e.starts("w")[-1]["env"]
        self.assertEqual(env["WORKSPACE_WORKER_ID"], "w")
        self.assertEqual(env["WORKSPACE_NOTIFY_SOCKET"], os.path.join(e.run_dir, "notify.sock"))
        self.assertIsNone(env["WORKSPACE_MANAGER_SOCKET"])
        # 임시 적용: 파일은 그대로, 다시 시작해서 새 값으로
        tmpcfg = {"mod": {"level": 3, "name": "a"}}
        d = e.ok({"op": "set_config", "id": "w", "config": tmpcfg, "save": False})
        self.assertEqual((d["temp"], d["config_version"]), (True, 2))
        s2 = e.wait_state("w", "running", applied_version=2)
        self.assertNotEqual(s2["pid"], s1["pid"])
        self.assertEqual(e.starts("w")[-1]["config"], tmpcfg)
        self.assertEqual(e.starts("w")[-1]["ver"], 2)
        self.assertEqual(read_json(os.path.join(e.ws["w"], "presets", "default.json"))["config"], base)
        self.assertEqual(read_json(os.path.join(e.run_dir, "temp_config.json"))["workers"], {"w": tmpcfg})
        d = e.call("manager.sock", {"op": "get_config", "id": "w"})["data"]
        self.assertEqual((d["config"], d["saved"], d["temp"], d["config_version"]), (tmpcfg, base, True, 2))
        # 같은 값 다시: 버전 그대로, 재시작 없음
        d = e.ok({"op": "set_config", "id": "w", "config": {"mod": {"name": "a", "level": 3}}, "save": False})
        self.assertEqual(d["config_version"], 2)
        # 저장된 값과 같게 임시 적용하면 temp 아님
        d = e.ok({"op": "set_config", "id": "w", "config": base, "save": False})
        self.assertEqual((d["temp"], d["config_version"]), (False, 3))
        self.assertEqual(read_json(os.path.join(e.run_dir, "temp_config.json"))["workers"], {})
        # 저장: 사용 중 프리셋 파일에(label 유지), temp 아님
        saved = {"mod": {"level": 4, "name": "b"}, "extra": [1, 2]}
        d = e.ok({"op": "set_config", "id": "w", "config": saved})
        self.assertEqual((d["temp"], d["config_version"]), (False, 4))
        self.assertEqual(read_json(os.path.join(e.ws["w"], "presets", "default.json")),
                         {"label": "기본", "config": saved})
        e.wait_state("w", "running", applied_version=4)
        self.assertEqual(read_json(os.path.join(e.data, "state", "versions.json"))["w"],
                         {"config_version": 4, "config": saved})
        # 프리셋 파일 모양: 들여쓰기 2, 줄바꿈으로 끝남
        with open(os.path.join(e.ws["w"], "presets", "default.json"), encoding="utf-8") as f:
            text = f.read()
        self.assertTrue(text.endswith("}\n"))
        self.assertIn('\n  "label": "기본"', text)

    def test_schema_rejection(self):
        e = self.env({"w": {"autostart": False, "decl": {"config_schema": SCHEMA}},  # 프리셋 파일 없음
                      "o": {"config": {"device_sn": "A1"}, "autostart": False}}).start()
        r = e.rest({"op": "set_config", "id": "w", "config": {"mod": {"level": 99, "peer": "Bad Id"}, "watchdog_sec": 0}})
        self.assertEqual(r["error"]["code"], "bad_request")
        self.assertEqual(set(r["error"]["fields"]), {"mod.level", "mod.peer", "watchdog_sec"})
        self.assertEqual(r["error"]["fields"]["mod.level"], "0 ~ 10 사이여야 합니다")
        self.assertEqual(r["error"]["fields"]["watchdog_sec"], "1 이상이어야 합니다")
        self.assertTrue(r["error"]["message"].startswith("설정 값이 맞지 않습니다: mod.level: 0 ~ 10 사이여야 합니다; "))
        r = e.rest({"op": "set_config", "id": "w", "config": {"mod": "x"}})
        self.assertEqual(r["error"]["fields"], {"mod": "객체여야 합니다"})
        r = e.rest({"op": "set_config", "id": "w", "config": [1]})
        self.assertEqual(r["error"], {"code": "bad_request", "message": "config must be an object"})
        r = e.rest({"op": "save_preset", "id": "w", "preset": {"name": "p2", "config": {"mod": {"level": -1}}}})
        self.assertEqual(r["error"]["code"], "bad_request")
        self.assertEqual(set(r["error"]["fields"]), {"mod.level"})
        self.assertFalse(os.path.exists(os.path.join(e.ws["w"], "presets", "p2.json")))
        self.assertEqual(e.status("w")["config_version"], 1)
        self.assertFalse(os.path.exists(os.path.join(e.ws["w"], "presets", "default.json")))
        # 같은 장치(device_sn)를 두 워커가 쓰면 conflict
        r = e.rest({"op": "set_config", "id": "w", "config": {"device_sn": "A1"}})
        self.assertEqual(r["error"], {"code": "conflict", "message": "device_sn 'A1' already used by 'o'"})
        # 맞는 값은 저장되고, 프리셋 파일이 없었으면 만든다(config.json은 있음)
        e.ok({"op": "set_config", "id": "w", "config": {"mod": {"level": 0, "peer": "o"}}})
        self.assertEqual(read_json(os.path.join(e.ws["w"], "presets", "default.json"))["label"], "기본")
        self.assertEqual(e.status("w")["peers"], {"mod.peer": "o"})

    def test_presets(self):
        e = self.env({"w": {"config": {"mod": {"level": 1}}, "autostart": False, "decl": {"config_schema": SCHEMA}}}).start()
        p = lambda n: os.path.join(e.ws["w"], "presets", n + ".json")
        cfgjson = lambda: read_json(os.path.join(e.ws["w"], "config.json"))
        # 새 이름으로 저장(기본 use): 지금 값으로, 사용 중으로
        d = e.ok({"op": "save_preset", "id": "w", "preset": {"name": "outdoor", "label": "야외"}})
        self.assertEqual(d["preset"], "outdoor")
        self.assertEqual(read_json(p("outdoor")), {"label": "야외", "config": {"mod": {"level": 1}}})
        self.assertEqual(cfgjson(), {"active": "outdoor", "autostart": False})
        r = e.rest({"op": "save_preset", "id": "w", "preset": {"name": "outdoor"}})
        self.assertEqual(r["error"], {"code": "conflict", "message": "preset 'outdoor' already exists"})
        r = e.rest({"op": "save_preset", "id": "w", "preset": {"name": "Bad Name"}})
        self.assertEqual(r["error"]["code"], "bad_request")
        self.assertIn("preset name must match", r["error"]["message"])
        # preset이 객체가 아님(문자열이면 Python 판은 명령 기록에서 죽어 답 없이 닫는다: 그 경우는 시험하지 않음)
        r = e.rest({"op": "save_preset", "id": "w", "preset": None})
        self.assertEqual(r["error"]["code"], "bad_request")
        r = e.rest({"op": "save_preset", "id": "w", "preset": {"name": "q", "config": 3}})
        self.assertEqual(r["error"], {"code": "bad_request", "message": "config must be an object"})
        # use: false면 만들기만, label 없으면 이름
        d = e.ok({"op": "save_preset", "id": "w", "preset": {"name": "b", "config": {"mod": {"level": 5}}, "use": False}})
        self.assertEqual(d["preset"], "outdoor")
        self.assertEqual(read_json(p("b")), {"label": "b", "config": {"mod": {"level": 5}}})
        d = e.call("manager.sock", {"op": "get_config", "id": "w"})["data"]
        self.assertEqual(d["presets"], [{"name": "b", "label": "b"}, {"name": "default", "label": "기본"},
                                        {"name": "outdoor", "label": "야외"}])
        # 이름 바꾸기
        d = e.ok({"op": "rename_preset", "id": "w", "name": "b", "preset": {"name": "c", "label": "씨"}})
        self.assertFalse(os.path.exists(p("b")))
        self.assertEqual(read_json(p("c")), {"label": "씨", "config": {"mod": {"level": 5}}})
        r = e.rest({"op": "rename_preset", "id": "w", "name": "c", "preset": {"name": "outdoor"}})
        self.assertEqual(r["error"], {"code": "conflict", "message": "preset 'outdoor' already exists"})
        r = e.rest({"op": "rename_preset", "id": "w", "name": "zz", "preset": {"name": "y"}})
        self.assertEqual(r["error"], {"code": "not_found", "message": "no preset 'zz'"})
        r = e.rest({"op": "rename_preset", "id": "w", "name": "c", "preset": {"name": "Bad!"}})
        self.assertEqual(r["error"]["code"], "bad_request")
        d = e.ok({"op": "rename_preset", "id": "w", "name": "outdoor", "preset": {"name": "out2"}})
        self.assertEqual(d["preset"], "out2")
        self.assertEqual(cfgjson()["active"], "out2")
        self.assertEqual(read_json(p("out2"))["label"], "야외")
        # 삭제
        r = e.rest({"op": "delete_preset", "id": "w", "name": "out2"})
        self.assertEqual(r["error"], {"code": "conflict",
                                      "message": "preset 'out2' is in use; switch to another preset first"})
        r = e.rest({"op": "delete_preset", "id": "w", "name": "zz"})
        self.assertEqual(r["error"], {"code": "not_found", "message": "no preset 'zz'"})
        e.ok({"op": "delete_preset", "id": "w", "name": "c"})
        self.assertFalse(os.path.exists(p("c")))
        # 프리셋 고르기: 그 값으로, config.json에 기록
        v = e.status("w")["config_version"]
        d = e.ok({"op": "use_preset", "id": "w", "name": "default"})
        self.assertEqual((d["preset"], d["config_version"]), ("default", v))  # 값이 같으면 버전 그대로
        self.assertEqual(cfgjson()["active"], "default")
        r = e.rest({"op": "use_preset", "id": "w", "name": "zz"})
        self.assertEqual(r["error"], {"code": "not_found", "message": "no preset 'zz'"})
        e.ok({"op": "save_preset", "id": "w", "preset": {"name": "hi", "config": {"mod": {"level": 9}}, "use": False}})
        d = e.ok({"op": "use_preset", "id": "w", "name": "hi"})
        self.assertEqual(d["config_version"], v + 1)
        d = e.call("manager.sock", {"op": "get_config", "id": "w"})["data"]
        self.assertEqual((d["config"], d["saved"], d["preset"]), ({"mod": {"level": 9}}, {"mod": {"level": 9}}, "hi"))

    def test_set_autostart(self):
        e = self.env({"w": {"config": {}, "autostart": False}, "n": {"config": {}}}).start()
        d = e.ok({"op": "set_autostart", "id": "w", "autostart": True})
        self.assertEqual((d["autostart"], d["desired_state"]), (True, "stopped"))  # 지금 실행 상태는 그대로
        self.assertEqual(read_json(os.path.join(e.ws["w"], "config.json")), {"active": "default", "autostart": True})
        d = e.ok({"op": "set_autostart", "id": "w", "autostart": {"autostart": False}})  # REST 본문 모양
        self.assertEqual(d["autostart"], False)
        r = e.rest({"op": "set_autostart", "id": "w", "autostart": "yes"})
        self.assertEqual(r["error"], {"code": "bad_request", "message": "autostart must be true or false"})
        # config.json이 없던 워커: 만든다
        e.ok({"op": "set_autostart", "id": "n", "autostart": True})
        self.assertEqual(read_json(os.path.join(e.ws["n"], "config.json")), {"active": "default", "autostart": True})

    # ---- 바로 적용(live_keys → SIGHUP) -------------------------------------

    def test_live_keys(self):
        base = {"mod": {"level": 1, "name": "a"}}
        e = self.env({"lv": {"config": base, "autostart": True,
                             "decl": {"config_schema": SCHEMA, "live_keys": ["mod.level"]}},
                      "nk": {"config": dict(base, behavior={"hup": "ignore"}), "autostart": True,
                             "decl": {"config_schema": SCHEMA, "live_keys": ["mod"]}}}).start()
        s1 = e.wait_state("lv", "running")
        n1 = e.wait_state("nk", "running")
        # live 키만: 다시 시작하지 않고 SIGHUP, 워커가 heartbeat로 확인
        e.ok({"op": "set_config", "id": "lv", "config": {"mod": {"level": 5, "name": "a"}}, "save": False})
        s2 = e.wait_state("lv", "running", applied_version=2, timeout=4)
        self.assertEqual(s2["pid"], s1["pid"])
        hups = [x for x in e.events("lv") if x["ev"] == "hup"]
        self.assertEqual(len(hups), 1)
        self.assertEqual((hups[0]["ver"], hups[0]["config"]), (2, {"mod": {"level": 5, "name": "a"}}))
        # live가 아닌 키: 다시 시작
        e.ok({"op": "set_config", "id": "lv", "config": {"mod": {"level": 5, "name": "b"}}, "save": False})
        s3 = e.wait_state("lv", "running", applied_version=3)
        self.assertNotEqual(s3["pid"], s1["pid"])
        # 워커가 확인하지 않으면(5 s) 다시 시작
        cfg = {"mod": {"level": 2, "name": "a"}, "behavior": {"hup": "ignore"}}
        t0 = time.time()
        e.ok({"op": "set_config", "id": "nk", "config": cfg, "save": False})
        time.sleep(1.5)
        mid = e.status("nk")
        self.assertEqual((mid["pid"], mid["applied_version"]), (n1["pid"], 1))
        self.assertTrue(wait_for(lambda: [x for x in e.events("nk") if x["ev"] == "hup"], 2))
        n2 = e.wait_state("nk", "running", applied_version=2, timeout=12)
        self.assertNotEqual(n2["pid"], n1["pid"])
        self.assertGreater(time.time() - t0, 4.5)

    # ---- 구독 -------------------------------------------------------------

    def test_subscribe(self):
        e = self.env({"a": {"config": {}, "autostart": True}, "b": {"config": {}, "autostart": False}}).start()
        e.wait_state("a", "running")
        sub = Subscriber(e.sock("manager-logger.sock"), {"op": "subscribe", "topics": ["workers", "system"], "period_ms": 300})
        try:
            time.sleep(1.0)
            lines = sub.snapshot()
            self.assertEqual(lines[0][1], {"ok": True, "data": {"subscribed": ["system", "workers"]}})
            evs = [x for _, x in lines[1:]]
            self.assertEqual({x["data"]["id"] for x in evs if x["event"] == "worker_status"}, {"a", "b"})
            sys_ev = [x for x in evs if x["event"] == "system"]
            self.assertTrue(sys_ev)
            self.assertEqual(sys_ev[-1]["data"]["clients"]["logger"], "subscribed")
            for x in evs:
                if x["event"] == "worker_status":
                    self.assertEqual(set(x["data"]), STATUS_KEYS)
            # 변화가 없어도 주기마다 전체
            n0 = len(sub.snapshot())
            time.sleep(1.5)
            per_b = [x for _, x in sub.snapshot()[n0:] if x["event"] == "worker_status" and x["data"]["id"] == "b"]
            self.assertGreaterEqual(len(per_b), 3)
            self.assertLessEqual(len(per_b), 8)
        finally:
            sub.close()
        # 변화는 바로(주기 5 s여도)
        sub = Subscriber(e.sock("manager-rest.sock"), {"op": "subscribe", "period_ms": 5000})
        cmd = Subscriber(e.sock("manager.sock"), {"op": "subscribe", "topics": ["commands"]})
        try:
            self.assertTrue(wait_for(lambda: len(sub.snapshot()) >= 4, 3))
            time.sleep(0.3)
            n0 = len(sub.snapshot())
            t0 = time.time()
            e.ok({"op": "start", "id": "b"})
            got = wait_for(lambda: [(t, x) for t, x in sub.snapshot()[n0:] if x.get("event") == "worker_status"
                                    and x["data"]["id"] == "b" and x["data"]["desired_state"] == "running"], 3)
            self.assertTrue(got)
            self.assertLess(got[0][0] - t0, 1.0)
            e.rest({"op": "set_autostart", "id": "b", "autostart": "bad"})
            c = wait_for(lambda: [x for _, x in cmd.snapshot() if x.get("event") == "command"][1:], 3)
            self.assertTrue(c, cmd.snapshot())
            first = [x for _, x in cmd.snapshot() if x.get("event") == "command"][0]["data"]
            self.assertEqual(first, {"op": "start", "id": "b", "client": "rest", "ok": True})
            self.assertEqual(c[0]["data"]["op"], "set_autostart")
            self.assertEqual(c[0]["data"]["autostart"], "bad")
            self.assertEqual(c[0]["data"]["ok"], False)
            self.assertEqual(c[0]["data"]["error"]["code"], "bad_request")
            self.assertEqual(cmd.snapshot()[0][1], {"ok": True, "data": {"subscribed": ["commands"]}})
        finally:
            sub.close()
            cmd.close()

    def test_slow_subscriber_dropped(self):
        metrics = {"m%02d_long_metric_name_padding" % i: 123456.789 for i in range(60)}
        e = self.env({n: {"config": {"behavior": {"metrics": metrics}}, "autostart": True} for n in ("a", "b", "c")},
                     settings={"api": {"subscriber_timeout_s": 1.0}}).start()
        for n in ("a", "b", "c"):
            e.wait_state(n, "running")
        slow = Subscriber(e.sock("manager-logger.sock"), {"op": "subscribe", "period_ms": 100}, read=False)
        fast = Subscriber(e.sock("manager-rest.sock"), {"op": "subscribe", "period_ms": 100})
        try:
            pings = []
            end = time.time() + 7
            while time.time() < end:
                t0 = time.time()
                self.assertTrue(e.call("manager.sock", {"op": "ping"})["ok"])
                pings.append(time.time() - t0)
                time.sleep(0.2)
            self.assertLess(max(pings), 0.5, pings)
            times = [t for t, _ in fast.snapshot()]
            self.assertGreater(len(times), 50)
            gaps = [b - a for a, b in zip(times, times[1:])]
            self.assertLess(max(gaps), 1.0)
            self.assertFalse(fast.closed)
            # 느린 구독자는 끊겼다: 쌓인 것을 읽으면 곧 끝(EOF)
            slow.s.settimeout(0.5)
            end = time.time() + 5
            eof = False
            while time.time() < end:
                try:
                    d = slow.s.recv(1 << 20)
                except socket.timeout:
                    continue
                except ConnectionResetError:
                    eof = True
                    break
                if not d:
                    eof = True
                    break
            self.assertTrue(eof, "slow subscriber was not dropped")
            sysst = e.call("manager.sock", {"op": "system"})["data"]
            self.assertEqual(sysst["clients"]["rest"], "subscribed")
            self.assertNotEqual(sysst["clients"]["logger"], "subscribed")
        finally:
            slow.close()
            fast.close()

    # ---- 다시 시작해도 상태 유지 ----------------------------------------

    def test_restore_after_restart(self):
        e = self.env({"r": {"config": {"mod": {"level": 1}}, "autostart": False},
                      "t": {"config": {"mod": {"level": 1}}, "autostart": False},
                      "auto": {"config": {}, "autostart": True}}).start()
        e.wait_state("auto", "running")
        e.ok({"op": "start", "id": "r"})
        e.ok({"op": "stop", "id": "auto"})
        e.ok({"op": "set_config", "id": "t", "config": {"mod": {"level": 7}}, "save": False})
        e.wait_state("r", "running")
        e.wait_state("auto", "stopped")
        pid_r = e.status("r")["pid"]
        e.stop()
        self.assertFalse(pid_alive(pid_r))  # Manager가 끝나면 워커도 정리
        # 같은 부팅: 직전 실행 상태·임시 값 복원
        e.start()
        e.wait_state("r", "running")
        st = e.status()
        self.assertEqual(st["auto"]["desired_state"], "stopped")
        self.assertEqual(st["t"]["desired_state"], "stopped")
        d = e.call("manager.sock", {"op": "get_config", "id": "t"})["data"]
        self.assertEqual((d["config"], d["saved"], d["temp"], d["config_version"]),
                         ({"mod": {"level": 7}}, {"mod": {"level": 1}}, True, 2))
        self.assertEqual(st["r"]["config_version"], 1)
        e.stop()
        # 다른 부팅(boot_id가 다름): autostart와 저장된 값으로
        for f in ("desired.json", "temp_config.json"):
            path = os.path.join(e.run_dir, f)
            obj = read_json(path)
            obj["boot_id"] = "another-boot"
            write_json(path, obj)
        e.start()
        e.wait_state("auto", "running")
        st = e.status()
        self.assertEqual(st["r"]["desired_state"], "stopped")
        d = e.call("manager.sock", {"op": "get_config", "id": "t"})["data"]
        self.assertEqual((d["config"], d["temp"], d["config_version"]), ({"mod": {"level": 1}}, False, 3))

    # ---- heartbeat·종료 ---------------------------------------------------

    def test_heartbeat_fields(self):
        e = self.env({"h": {"config": {"behavior": {"sim": True, "metrics": {"x_fps": 30, "x_ratio": 0.5,
                                                                               "x_name": "abc", "x_neg": -2}}},
                            "autostart": True}}).start()
        s = wait_for(lambda: (lambda w: w if w["metrics"] else None)(e.status("h")), 10)
        self.assertTrue(s)
        self.assertEqual(s["metrics"], {"x_fps": 30, "x_ratio": 0.5, "x_name": "abc", "x_neg": -2})
        self.assertEqual((s["libworker"], s["sim"], s["actual_state"]), ("test-1", True, "running"))
        self.assertIsInstance(s["progress"], int)
        self.assertGreater(s["progress"], 0)
        self.assertIsInstance(s["last_heartbeat_ms_ago"], int)
        self.assertLess(s["last_heartbeat_ms_ago"], 1500)
        self.assertRegex(s["started_at"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d[+-]\d\d:\d\d$")
        self.assertEqual(s["exec"], sys.executable)
        r = wait_for(lambda: e.status("h")["resources"] and e.status("h")["resources"]["cpu_pct"] is not None, 5)
        self.assertTrue(r)
        res = e.status("h")["resources"]
        self.assertEqual(set(res), {"cpu_pct", "rss_mb", "procs", "threads"})
        self.assertGreater(res["rss_mb"], 1)
        self.assertEqual(res["procs"], 1)
        # 정지하면 지표·자원은 비운다
        e.ok({"op": "stop", "id": "h"})
        s = e.wait_state("h", "stopped")
        self.assertEqual((s["metrics"], s["resources"], s["pid"], s["last_exit_code"]), (None, None, None, 0))

    def test_stop_timeout_kills_group(self):
        e = self.env({"k": {"config": {"behavior": {"term": "ignore", "child": True}}, "autostart": True,
                            "decl": {"stop_timeout_sec": 1}},
                      "c": {"config": {"behavior": {"child": True}}, "autostart": True}}).start()
        sk = e.wait_state("k", "running")
        e.wait_state("c", "running")
        child_k = wait_for(lambda: [x["pid"] for x in e.events("k") if x["ev"] == "child"])[0]
        child_c = wait_for(lambda: [x["pid"] for x in e.events("c") if x["ev"] == "child"])[0]
        t0 = time.time()
        e.ok({"op": "stop", "id": "k"})
        e.ok({"op": "stop", "id": "c"})
        time.sleep(0.5)
        self.assertTrue(pid_alive(sk["pid"]))  # SIGTERM을 무시하는 중
        s = e.wait_state("k", "stopped", timeout=5)
        self.assertGreater(time.time() - t0, 0.9)
        self.assertEqual((s["last_signal"], s["last_exit_code"]), ("SIGKILL", None))
        self.assertFalse(pid_alive(sk["pid"]))
        self.assertTrue(wait_for(lambda: not pid_alive(child_k), 3))
        # 정상 종료(exit 0)한 워커의 남은 자식도 정리
        s = e.wait_state("c", "stopped")
        self.assertEqual((s["last_exit_code"], s["last_signal"]), (0, None))
        self.assertTrue(wait_for(lambda: not pid_alive(child_c), 3))

    def test_exit_reasons(self):
        e = self.env({
            "e0": {"config": {"behavior": {"exit": 0}}, "autostart": True},
            "e1": {"config": {"behavior": {"exit": 1}}, "autostart": True},
            "e3": {"config": {"behavior": {"exit": 3}}, "autostart": True},
            "seg": {"config": {"behavior": {"exit": 0, "kill_self": "SIGSEGV"}}, "autostart": True},
            "nr": {"config": {"behavior": {"ready": False}}, "autostart": True, "decl": {"start_timeout_sec": 1}},
            "nh": {"config": {"behavior": {"heartbeat": False}}, "autostart": True, "decl": {"watchdog_sec": 1.5}},
            "nx": {"config": {}, "autostart": True, "decl": {"exec": ["/no/such/binary"]}},
        }, settings={"restart": {"interval_s": 0.5, "max_interval_s": 0.5}}).start()
        want = {"e0": "unexpected exit 0", "e1": "config error (exit 1)", "e3": "exit 3",
                "seg": "killed by SIGSEGV", "nr": "stalled: no READY in 1s", "nh": "stalled: no heartbeat for 1.5s"}
        got = wait_for(lambda: (lambda st: st if all(st[k]["last_error"] == v and st[k]["restart_count"] >= 1
                                                     for k, v in want.items()) else None)(e.status()), 20)
        self.assertTrue(got, {k: e.status(k)["last_error"] for k in want})
        self.assertEqual((got["seg"]["last_signal"], got["seg"]["last_exit_code"]), ("SIGSEGV", None))
        self.assertEqual((got["e3"]["last_exit_code"], got["e3"]["last_signal"]), (3, None))
        self.assertTrue(got["nx"]["last_error"].startswith("spawn failed: "), got["nx"])
        for k in want:
            self.assertGreaterEqual(got[k]["restart_count"], 1, k)
        # 대기 중 stop이면 stopped, start면 처음부터
        e.ok({"op": "stop", "id": "e1"})
        s = e.wait_state("e1", "stopped")
        self.assertEqual(s["restart_in_s"], None)
        d = e.ok({"op": "start", "id": "e1"})
        self.assertEqual((d["restart_count"], d["crash_loop"]), (0, False))

    # ---- 로그 ---------------------------------------------------------------

    def test_logs_and_rotation(self):
        e = self.env({"lg": {"config": {"behavior": {"log_bytes": 5000}}, "autostart": True}},
                     settings={"log": {"max_bytes": 2000, "backups": 2}}).start()
        log = os.path.join(e.data, "state", "logs", "lg.log")
        e.wait_state("lg", "running")
        self.assertTrue(wait_for(lambda: os.path.exists(log + ".1"), 15), os.listdir(os.path.dirname(log)))
        with open(log + ".1") as f:
            self.assertIn("hello stdout from lg", f.read())
        self.assertLessEqual(os.path.getsize(log), 2000)
        # 다시 시작해도 같은 파일에 이어 씀(O_APPEND)
        e.ok({"op": "set_config", "id": "lg", "config": {"behavior": {"log_bytes": 10}}, "save": False})
        e.wait_state("lg", "running", applied_version=2)
        self.assertTrue(wait_for(lambda: "hello stdout from lg" in open(log).read(), 5))

    # ---- 워커 폴더 ----------------------------------------------------------

    def test_worker_folders(self):
        legacy = {"old-id": {"type": "leg_t", "config": {"a": 1}, "autostart": True}}
        e = self.env({"leg": {}, "brk": {"raw_config_json": "{not json"}, "nopreset": {"autostart": False}},
                     builtins={"mon": ("mon_t", {"builtin": {"id": "mon", "autostart": True, "config": {"k": 1}}}),
                               "tool": ("tool_t", {"builtin": {"id": "tool", "autostart": True},
                                                   "requires": ["no-such-tool-xyz"]})},
                     legacy=legacy)
        os.makedirs(os.path.join(e.data, "run"), exist_ok=True)
        os.makedirs(os.path.join(e.data, "state", "workers"), exist_ok=True)
        stale = [os.path.join(e.data, "run", "manager-old.sock"), os.path.join(e.data, "state", "workers", "ghost.json")]
        for f in stale:
            open(f, "w").close()
        e.start()
        # 예전 workers.json → 워커 폴더로 한 번 옮김
        self.assertEqual(read_json(os.path.join(e.ws["leg"], "presets", "default.json")), {"label": "기본", "config": {"a": 1}})
        self.assertEqual(read_json(os.path.join(e.ws["leg"], "config.json")), {"active": "default", "autostart": True})
        self.assertTrue(os.path.exists(os.path.join(e.data, "config", "workers.json.moved")))
        self.assertFalse(os.path.exists(os.path.join(e.data, "config", "workers.json")))
        e.wait_state("leg", "running")
        # 깨진 설정 파일: 그 워커만 error, 명령은 conflict
        s = e.status("brk")
        self.assertEqual(s["actual_state"], "error")
        self.assertTrue(s["last_error"].startswith("설정 파일: "), s["last_error"])
        r = e.rest({"op": "start", "id": "brk"})
        self.assertEqual(r["error"]["code"], "conflict")
        self.assertIn("fix the file in the worker folder", r["error"]["message"])
        # 기본 탑재: builtin 표시, type 선언 기본값으로 돈다(@WORKSPACE_ROOT@)
        s = e.wait_state("mon", "running")
        self.assertTrue(s["builtin"])
        self.assertEqual(e.starts("mon")[-1]["config"], {"k": 1})
        d = e.call("manager.sock", {"op": "get_config", "id": "mon"})["data"]
        self.assertEqual(d["files"], os.path.join(e.root, "mon"))
        # 필요 도구가 없으면 disabled
        s = e.status("tool")
        self.assertEqual((s["actual_state"], s["last_error"], s["desired_state"]),
                         ("disabled", "needs no-such-tool-xyz", "stopped"))
        r = e.rest({"op": "start", "id": "tool"})
        self.assertEqual(r["error"], {"code": "conflict",
                                      "message": "worker cannot run on this machine (needs no-such-tool-xyz)"})
        e.ok({"op": "stop", "id": "tool"})
        self.assertEqual(e.status("tool")["actual_state"], "disabled")
        # 프리셋 파일이 없는 워커: 빈 설정으로 시작 가능, 저장하면 파일이 생김
        e.ok({"op": "start", "id": "nopreset"})
        e.wait_state("nopreset", "running")
        # 지금 구성에 없는 데이터는 정리
        for f in stale:
            self.assertFalse(os.path.exists(f), f)
        self.assertFalse(os.path.exists(os.path.join(e.data, "state", "workers", "brk.json")))


if __name__ == "__main__":
    unittest.main()
