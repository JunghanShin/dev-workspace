"""Manager API를 쓰는 워커 시험(design.md 6.5). Linux(PC WSL·보드)에서. 표준 라이브러리만.

    cd manager && python3 -m unittest test_manager_api -v
    MANAGER_BIN=~/pm-build/process-manager python3 -m unittest test_manager_api -v   # C++ 판

임시 워크스페이스(워커 폴더 둘)와 데이터 폴더를 만들고 진짜 Manager를 띄운다:
  - adp: Manager API를 쓰는 워커(워커 선언 "manager_api": true). Manager 소켓(WORKSPACE_MANAGER_SOCKET)으로 명령을 보내고 결과를 파일에 쓴다
  - dev: 일반 워커(자동 시작 안 함)
확인: Manager API를 쓰는 워커가 다른 워커를 켬, 자기 자신은 못 건드림, 일반 워커에는 Manager 소켓이 없음,
재시작 간격 늘림·연속 실패 표시·워커 프로세스 우선순위(nice, design.md 6.6.9 ②③),
읽기 전용 소켓(manager.sock, logger)의 쓰기는 거부. 예전 설정(config/mode, manager.json "control")은 무시
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

if not os.environ.get("MANAGER_BIN"):  # 0.58부터 C++ process-manager만(Python 판 삭제)
    raise unittest.SkipTest("MANAGER_BIN(process-manager 실행 파일)이 있어야 함")

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, "..", "workers", "_template", "python", "workspace_worker.py")

DEV = r'''
import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import workspace_worker as ww
ww.load_config()
open(sys.argv[3], "w").write(os.environ.get("WORKSPACE_MANAGER_SOCKET", "-"))
hb = ww.Heartbeat(); stop = ww.StopFlag(); hb.ready()
while not stop.requested:
    hb.watchdog(1); time.sleep(0.2)
'''

ADP = r'''
import json, os, socket, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import workspace_worker as ww
ww.load_config()
def call(req):
    s = socket.socket(socket.AF_UNIX); s.settimeout(2); s.connect(os.environ["WORKSPACE_MANAGER_SOCKET"])
    s.sendall((json.dumps(req) + "\n").encode()); data = b""
    while not data.endswith(b"\n"):
        d = s.recv(65536)
        if not d: break
        data += d
    return json.loads(data)
hb = ww.Heartbeat(); stop = ww.StopFlag(); hb.ready()
# 관리 조작은 개발 모드에서만(docs/command-path.md 3장 ③): 시험이 개발 모드로 바꿀 때까지 기다림
while call({"op": "system"})["data"].get("access_mode") != "development" and not stop.requested:
    hb.watchdog(1); time.sleep(0.1)
out = {"system": call({"op": "system"}), "start_dev": call({"op": "start", "id": "dev"}),
       "stop_self": call({"op": "stop", "id": "adp"}), "list": call({"op": "list"})}
open(sys.argv[3], "w").write(json.dumps(out))
while not stop.requested:
    hb.watchdog(1); time.sleep(0.2)
'''


def manager_cmd():
    """시험할 Manager 실행 명령. MANAGER_BIN이 있으면 그 실행 파일(C++ process-manager), 없으면 Python 판"""
    b = os.environ.get("MANAGER_BIN")
    return [b]


def wait_for(fn, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return None


@unittest.skipUnless(sys.platform.startswith("linux"), "Unix sockets, Linux only")
class ManagerApiWorker(unittest.TestCase):
    def start(self, product=False, crash=False):
        self.tmp = tempfile.mkdtemp(prefix="ws-mgrapi-")
        root, data = os.path.join(self.tmp, "ws"), os.path.join(self.tmp, "data")
        self.out = {n: os.path.join(self.tmp, n + ".out") for n in ("adp", "dev", "crash")}
        workers = [("adp", ADP, True, True), ("dev", DEV, False, False)]
        if crash:
            workers.append(("crash", "import sys\nsys.exit(2)\n", False, True))  # 띄우자마자 실패
        for name, src, api, autostart in workers:
            ws = os.path.join(root, "workers", name, "workspace")
            os.makedirs(os.path.join(ws, "presets"))
            shutil.copyfile(HELPER, os.path.join(ws, "workspace_worker.py"))
            with open(os.path.join(ws, "w.py"), "w") as f:
                f.write(src)
            json.dump({"active": "default", "autostart": autostart}, open(os.path.join(ws, "config.json"), "w"))
            cfg = {"sim": {}} if (product and name == "dev") else {}  # 제품에서 가짜 장치 설정은 거부돼야 함
            with open(os.path.join(ws, "presets", "default.json"), "w") as f:
                json.dump({"label": "기본", "config": cfg}, f)
            decl = {"exec": [sys.executable, os.path.join(ws, "w.py"), "-", self.out[name]],
                    "start_timeout_sec": 10, "watchdog_sec": 5, "stop_timeout_sec": 3}
            if api:
                decl["manager_api"] = True
            if name == "dev":
                decl["process"] = {"nice": 5}  # 워커 프로세스 우선순위(올리는 것은 권한 없이 됨)
            else:  # 실행 파일 후보: 없는 첫째는 건너뛰고 있는 것을 쓴다(개발 빌드 build/ → 확정 workspace/bin/)
                decl["exec_candidates"] = [os.path.join(self.tmp, "no-such-build", "python3"), sys.executable]
            os.makedirs(os.path.join(data, "config", "worker-types.d"), exist_ok=True)
            json.dump({name + "_t": decl}, open(os.path.join(data, "config", "worker-types.d", name + ".json"), "w"))
        # 입출력 정의(design.md 6.6.7.1): dev 워커에 역할·워커 정의, 틀에 공통 정의 → get_io
        dev_dir = os.path.join(root, "workers", "dev")
        os.makedirs(os.path.join(dev_dir, "io", "uses"))
        os.makedirs(os.path.join(dev_dir, "lib", "libx", "io"))
        json.dump({"provides": ["dev_role"], "uses": {"up": {"from": "up_role", "version": "1.0", "required": False}}},
                  open(os.path.join(dev_dir, "io.json"), "w"))
        json.dump({"name": "dev_role", "version": "1.0", "topics": {}}, open(os.path.join(dev_dir, "io", "dev_role.json"), "w"))
        json.dump({"name": "up_role", "version": "1.0"}, open(os.path.join(dev_dir, "io", "uses", "up_role.json"), "w"))
        json.dump({"name": "self", "version": "1.0", "include": ["lib/libx/io/x.json"], "status": {}},
                  open(os.path.join(dev_dir, "io", "self.json"), "w"))
        json.dump({"name": "x", "version": "1.0", "metrics": {}}, open(os.path.join(dev_dir, "lib", "libx", "io", "x.json"), "w"))
        tio = os.path.join(root, "workers", "_template", "lib", "libworker", "io")
        os.makedirs(tio)
        shutil.copyfile(os.path.join(HERE, "..", "workers", "_template", "lib", "libworker", "io", "common.json"),
                        os.path.join(tio, "common.json"))
        # 예전 구성이 남긴 데이터: Manager가 지워야 한다(예전 이름 소켓, 빠진 워커의 실행 설정 파일)
        os.makedirs(os.path.join(data, "run"), exist_ok=True)
        os.makedirs(os.path.join(data, "state", "workers"), exist_ok=True)
        self.stale = [os.path.join(data, "run", "manager-ops.sock"), os.path.join(data, "state", "workers", "ghost-0.json")]
        for f in self.stale:
            open(f, "w").close()
        # 예전 보드에 남은 설정: 무시되어야 한다(운용 모드·제어권 없음, design.md 6.1)
        open(os.path.join(data, "config", "mode"), "w").write("operation\n")
        mj = {"control": {"operation": "rest"}}
        if crash:  # 시험이 빨리 끝나게 짧은 간격
            mj["restart"] = {"interval_s": 0.2, "max_interval_s": 0.8, "crash_count": 3, "crash_window_s": 60}
        json.dump(mj, open(os.path.join(data, "config", "manager.json"), "w"))
        # 워커 실행 인자: exec 뒤에 --config <파일>이 붙는다. w.py는 load_config가 argv에서 --config만 읽도록 "-"를 둔다
        for name in ("adp", "dev"):  # crash는 인자를 읽지 않음
            p = os.path.join(root, "workers", name, "workspace", "w.py")
            s = open(p).read().replace("ww.load_config()", "ww.load_config(argv=sys.argv[sys.argv.index('--config'):])")
            open(p, "w").write(s.replace("sys.argv[3]", "sys.argv[2]"))
        env = dict(os.environ, WORKSPACE_ROOT=root, WORKSPACE_DATA=data, PYTHONPATH=HERE, PYTHONDONTWRITEBYTECODE="1",
                   WORKSPACE_PRODUCT="1" if product else "0")
        env.pop("WORKSPACE_CONFIG_WRITE", None)
        self.mgr = subprocess.Popen(manager_cmd() + ["--data", data], env=env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.run = os.path.join(data, "run")
        self.sock = os.path.join(self.run, "manager.sock")

    def tearDown(self):
        self.mgr.terminate()
        try:
            self.mgr.wait(10)
        except subprocess.TimeoutExpired:
            self.mgr.kill()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def read(self, name):
        try:
            return open(self.out[name]).read()
        except OSError:
            return None

    def call(self, path, req):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(2)
            s.connect(path)
            s.sendall((json.dumps(req) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                d = s.recv(65536)
                if not d:
                    break
                data += d
        return json.loads(data)

    def status(self):
        return {w["id"]: w for w in self.call(self.sock, {"op": "list"})["data"]}

    def test_manager_api_worker(self):
        self.start()
        rest = os.path.join(self.run, "manager-rest.sock")
        self.assertTrue(wait_for(lambda: os.path.exists(rest)))
        time.sleep(0.5)
        self.assertIsNone(self.read("adp"))  # 운용 모드: 아직 기다리는 중
        self.assertTrue(self.call(rest, {"op": "set_access_mode", "mode": "development"})["ok"])
        out = json.loads(wait_for(lambda: self.read("adp")) or "null")
        self.assertIsNotNone(out, "manager_api worker did not run")
        self.assertEqual(out["system"]["data"]["manager_api_workers"], ["adp"])
        self.assertIn("adp", out["system"]["data"]["clients"])
        self.assertNotIn("mode", out["system"]["data"])
        self.assertTrue(out["start_dev"]["ok"], out["start_dev"])
        self.assertEqual(out["stop_self"]["error"]["code"], "forbidden")
        self.assertTrue(out["list"]["ok"])
        # dev가 실제로 뜨고, 일반 워커에는 Manager 소켓이 없다
        self.assertEqual(wait_for(lambda: self.read("dev")), "-")
        self.assertTrue(wait_for(lambda: self.status()["dev"]["actual_state"] == "running"))
        self.assertEqual(self.status()["dev"]["exec"], sys.executable)  # 있는 후보로 띄움
        for f in self.stale:  # 지금 구성에 없는 데이터는 정리됨
            self.assertFalse(os.path.exists(f), f)

    def test_restart_backoff_and_crash_loop(self):
        self.start(crash=True)
        self.assertTrue(wait_for(lambda: os.path.exists(self.sock)))
        st = wait_for(lambda: (lambda w: w if w.get("crash_loop") else None)(self.status().get("crash", {})), 20)
        self.assertTrue(st, "crash loop flagged after 3 quick failures")
        self.assertGreaterEqual(st["restart_count"], 3)
        self.assertEqual(st["last_error"], "exit 2")
        # 간격은 늘었다가 max_interval_s(0.8)에서 멈춘다
        seen = []
        end = time.time() + 4
        while time.time() < end:
            v = self.status()["crash"].get("restart_in_s")
            if v is not None:
                seen.append(v)
            time.sleep(0.05)
        self.assertTrue(seen and max(seen) <= 0.8 and max(seen) > 0.2, seen)
        # start 명령이면 처음부터(연속 실패 표시 지움). 재시작 대기 중일 때 보낸다(프로세스가 떠 있는 짧은 순간에는
        # 지우지 않는 규칙이라, 그때 보내면 두 판 모두 crash_loop가 남는다)
        self.assertTrue(wait_for(lambda: self.status()["crash"].get("restart_in_s") is not None, 5))
        r = self.call(os.path.join(self.run, "manager-rest.sock"), {"op": "start", "id": "crash"})
        self.assertTrue(r["ok"], r)
        self.assertFalse(r["data"]["crash_loop"])

    def test_process_priority(self):
        self.start()
        self.assertTrue(wait_for(lambda: os.path.exists(self.sock)))
        r = self.call(os.path.join(self.run, "manager-rest.sock"), {"op": "start", "id": "dev"})
        self.assertTrue(r["ok"], r)
        st = wait_for(lambda: (lambda w: w if w.get("actual_state") == "running" else None)(self.status()["dev"]))
        self.assertTrue(st)
        self.assertEqual(st["process"], {"nice": 5})
        with open("/proc/%d/stat" % st["pid"]) as f:
            nice = int(f.read().rsplit(")", 1)[1].split()[16])  # 19번째 칸(nice)
        self.assertEqual(nice, 5)

    def test_read_only_sockets(self):
        self.start()
        self.assertTrue(wait_for(lambda: os.path.exists(os.path.join(self.run, "manager-rest.sock"))))
        for name in ("manager.sock", "manager-logger.sock"):
            r = self.call(os.path.join(self.run, name), {"op": "start", "id": "dev"})
            self.assertEqual(r["error"]["code"], "forbidden", name)
        r = self.call(os.path.join(self.run, "manager-rest.sock"), {"op": "set_autostart", "id": "dev", "autostart": False})
        self.assertTrue(r["ok"], r)  # REST는 쓰기 가능



    def test_get_io(self):
        """입출력 정의 읽기(읽기 전용 소켓에서도 됨). 정의가 없는 워커는 not_found"""
        self.start()
        self.assertTrue(wait_for(lambda: os.path.exists(self.sock)))
        r = self.call(self.sock, {"op": "get_io", "id": "dev"})
        self.assertTrue(r["ok"], r)
        d = r["data"]
        self.assertEqual(d["common"]["name"], "common")
        self.assertEqual(d["self"]["name"], "self")
        self.assertEqual(d["includes"]["lib/libx/io/x.json"]["name"], "x")
        self.assertEqual(d["provides"]["dev_role"]["name"], "dev_role")
        self.assertEqual(d["uses"]["up"]["definition"]["name"], "up_role")
        r = self.call(self.sock, {"op": "get_io", "id": "adp"})
        self.assertEqual(r["error"]["code"], "not_found", r)

    def test_product_mode(self):
        """제품 모드(WORKSPACE_PRODUCT=1): 설정 잠금(시작·정지는 됨), 가짜 장치 설정 워커는 띄우지 않음, 상태에 표시"""
        self.start(product=True)
        self.assertTrue(wait_for(lambda: os.path.exists(os.path.join(self.run, "manager-rest.sock"))))
        rest = os.path.join(self.run, "manager-rest.sock")
        sysst = self.call(rest, {"op": "system"})["data"]
        self.assertTrue(sysst["product"])
        self.assertFalse(sysst["config_write"])
        r = self.call(rest, {"op": "set_autostart", "id": "dev", "autostart": False})
        self.assertEqual(r["error"]["code"], "forbidden")  # 설정 잠금
        r = self.call(rest, {"op": "set_config", "id": "dev", "config": {}, "save": False})
        self.assertEqual(r["error"]["code"], "forbidden")
        r = self.call(rest, {"op": "start", "id": "dev"})
        self.assertTrue(r["ok"], r)  # 시작은 된다
        st = wait_for(lambda: self.status()["dev"]["actual_state"] == "error" and self.status()["dev"])
        self.assertTrue(st, self.status()["dev"])
        self.assertIn("sim", st["last_error"])  # 가짜 장치 설정이라 띄우지 않음

if __name__ == "__main__":
    unittest.main()
