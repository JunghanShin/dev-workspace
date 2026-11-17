"""화면의 "제품 빌드"(design.md 11.4): 보드가 스스로 제품 묶음을 만들고 내려받게 한다. 개발 모드에서만(제품 모드면 404).

빌드 자체는 dev/build_product.py(PC 명령과 같은 코드)를 --local로 자식 프로세스로 돌리고, 그 표준 출력의
"##PROGRESS <0~100> <글>", "##RESULT <JSON>", "##ERROR <글>" 줄로 진행 상황을 만든다. 한 번에 하나만.
결과는 <데이터>/dist/에 마지막 하나만 둔다(--replace).

  GET    /system/product-build                 {state, profile, pct, step, started, finished, elapsed, error, result,
                                                log[], last{file, mb, created}, profiles[]}
  GET    /system/product-build/check?profile=  빌드 전 점검 {ok, items[{key, label, ok, detail}]}
  POST   /system/product-build  {"profile", "power_mode"?}  시작(202). 진행 중이면 409. power_mode: 장비 설치가 맞출 전원 모드
                                                (예: MAXN_SUPER, keep). 없으면 product.json
  DELETE /system/product-build                 취소
  GET    /system/product-build/download        마지막 묶음 파일(첨부)
"""
import glob
import json
import logging
import os
import signal
import subprocess
import sys
import threading
import time

from . import reads

log = logging.getLogger("workspace.rest.productbuild")

SCRIPT = os.path.join(reads.WORKSPACE, "dev", "build_product.py")
LOG_LINES = 300


def _env():
    """자식 Python의 입출력을 UTF-8로(서비스 환경은 LANG이 없어 한글 진행 글이 깨진다)"""
    return dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1")


class Busy(Exception):
    pass


class Job:
    def __init__(self, data_dir):
        self.out = os.path.join(data_dir or "/tmp", "dist")
        self.lock = threading.Lock()
        self.proc = None
        self.st = {"state": "idle"}
        self.log = []
        self._profiles = (0.0, [])

    # ---- 읽기 ----
    def _py(self, *args, timeout=60):
        r = subprocess.run([sys.executable, SCRIPT] + list(args), capture_output=True, timeout=timeout, env=_env(),
                           encoding="utf-8", errors="replace")
        return r

    def profiles(self):
        """product.json이 있는 프로필(화면은 진행 중 1초마다 상태를 읽으므로 30초 묶어 둔다)"""
        now = time.time()
        if now - self._profiles[0] > 30:
            try:
                self._profiles = (now, json.loads(self._py("--profiles").stdout or "[]"))
            except (ValueError, subprocess.SubprocessError, OSError) as e:
                log.error("profiles: %s", e)
                return []
        return self._profiles[1]

    def check(self, profile):
        if not profile:
            raise ValueError("profile is required")
        r = self._py(profile, "--check", "--json", "--local", "--out", self.out)
        try:
            return json.loads(r.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            msg = (r.stdout + r.stderr).strip().splitlines()
            return {"ok": False, "profile": profile,
                    "items": [{"key": "error", "label": "점검", "ok": False, "detail": msg[-1] if msg else "failed"}]}

    def last(self):
        files = sorted(glob.glob(os.path.join(self.out, "*.tar.gz")), key=os.path.getmtime)
        if not files:
            return None
        f = files[-1]
        return {"file": os.path.basename(f), "path": f, "mb": round(os.path.getsize(f) / 1e6, 1),
                "created": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(os.path.getmtime(f)))}

    def status(self):
        with self.lock:
            s = dict(self.st)
            s["log"] = list(self.log[-80:])
        if s.get("started"):
            s["elapsed"] = round((s.get("finished") or time.time()) - s["started"], 1)
        s["last"] = self.last()
        s["profiles"] = self.profiles()
        return s

    # ---- 시작·취소 ----
    def start(self, profile, power_mode=None):
        with self.lock:
            if self.st.get("state") == "running":
                raise Busy("a build is already running (%s)" % self.st.get("profile"))
            os.makedirs(self.out, exist_ok=True)
            self.log = []
            self.st = {"state": "running", "profile": profile, "power_mode": power_mode, "pct": 0, "step": "시작",
                       "started": time.time(), "finished": None, "error": None, "result": None}
            cmd = [sys.executable, "-u", SCRIPT, profile, "--local", "--replace", "--out", self.out]
            if power_mode:
                cmd += ["--power-mode", power_mode]
            self.proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=1, env=_env(),
                encoding="utf-8", errors="replace",
                start_new_session=True)  # 취소할 때 Nuitka·gcc 자식까지 한 번에(프로세스 그룹)
        threading.Thread(target=self._read, args=(self.proc,), name="product-build", daemon=True).start()
        log.info("product build started: %s", profile)
        return self.status()

    def _read(self, proc):
        for line in proc.stdout:
            line = line.rstrip("\n")
            with self.lock:
                if line.startswith("##PROGRESS "):
                    parts = line.split(" ", 2)
                    try:
                        self.st["pct"] = int(parts[1])
                    except (IndexError, ValueError):
                        pass
                    self.st["step"] = parts[2] if len(parts) > 2 else ""
                    self.log.append("· " + self.st["step"])
                elif line.startswith("##RESULT "):
                    try:
                        self.st["result"] = json.loads(line[9:])
                    except ValueError:
                        pass
                elif line.startswith("##ERROR "):
                    self.st["error"] = line[8:]
                elif line.strip():
                    self.log.append(line)
                del self.log[:-LOG_LINES]
        code = proc.wait()
        with self.lock:
            self.st["finished"] = time.time()
            if self.st.get("state") == "cancelled":
                pass
            elif code == 0 and self.st.get("result"):
                self.st["state"] = "done"
            else:
                self.st["state"] = "failed"
                self.st["error"] = self.st.get("error") or "exit %d" % code
        log.info("product build %s (%s)", self.st["state"], self.st.get("error") or "")

    def cancel(self):
        with self.lock:
            if self.st.get("state") != "running" or not self.proc:
                return False
            self.st["state"] = "cancelled"
            self.st["error"] = "취소됨"
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        return True


def send_file(handler, path):
    """큰 파일을 조각으로 보낸다(묶음 약 90 MB)"""
    size = os.path.getsize(path)
    handler.send_response(200)
    handler.send_header("Content-Type", "application/gzip")
    handler.send_header("Content-Length", str(size))
    handler.send_header("Content-Disposition", 'attachment; filename="%s"' % os.path.basename(path))
    handler.send_header("Cache-Control", "no-store")
    handler.end_headers()
    handler.connection.settimeout(60)
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            handler.wfile.write(chunk)
