"""게이트웨이 수락 시험(HTTP 블랙박스, 표준 라이브러리만, Linux). C++ 게이트웨이(docs/gateway.md)의 바깥 동작을 본다.

    cd rest && REST_BIN=<게이트웨이 실행 파일> python3 -m unittest test_rest_accept -v

가짜 Manager 소켓·가짜 워커(명령·스트림 소켓)·임시 워크스페이스/데이터 폴더를 만들고, 서버를 같은 인자·환경 변수로 띄운다.
정답이 복잡한 읽기는 기준 코드와 비교한다: 워크스페이스 정보는 reference_profile.py(옛 Python REST의 것, 시험 기준으로만 남김),
지난 지표는 logger의 workspace_history, 로그 끝부분은 아래 tail(). REST_BIN이 없으면 전부 건너뛴다(0.54부터 Python REST 없음).
"""
import datetime
import http.client
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
REPO = os.path.dirname(HERE)
REST_BIN = os.environ.get("REST_BIN")
if not REST_BIN:
    raise unittest.SkipTest("REST_BIN(C++ 게이트웨이 실행 파일)이 있어야 함")
START_TIMEOUT = float(os.environ.get("REST_START_TIMEOUT", "20"))
COMMAND_TIMEOUT = 0.5

T = {}  # 모듈 전체가 쓰는 임시 폴더·가짜들(setUpModule)


# ---- 도우미 -----------------------------------------------------------------------------------

def wait_for(cond, timeout=5.0, step=0.02):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(step)
    return cond()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(path, mode, **({} if mode == "wb" else {"encoding": "utf-8", "newline": ""})) as f:
        f.write(data)


CRLF = bytes([13, 10])
CRLF2 = CRLF + CRLF


def raw_request(port, data, timeout=5.0):
    """원시 요청 → (상태 코드, 본문). 응답이 끝날 때(연결 닫힘·Content-Length)까지 읽는다"""
    s = socket.create_connection(("127.0.0.1", port), timeout)
    try:
        s.sendall(data)
        buf = b""
        while True:
            if CRLF2 in buf:
                head, body = buf.split(CRLF2, 1)
                for line in head.split(CRLF)[1:]:
                    k, _, v = line.partition(b":")
                    if k.strip().lower() == b"content-length" and len(body) >= int(v):
                        return int(head.split()[1]), body[:int(v)]
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        head, body = buf.split(CRLF2, 1)
        return int(head.split()[1]), body
    finally:
        s.close()


class UnixServer:
    """Unix 소켓 서버(연결마다 스레드). handle(conn)은 하위 클래스가 정한다."""

    def __init__(self, path):
        self.path = path
        if os.path.exists(path):
            os.unlink(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(16)
        self.srv.settimeout(0.1)
        self.stop = False
        self.t = threading.Thread(target=self._accept, daemon=True)
        self.t.start()

    def _accept(self):
        while not self.stop:
            try:
                c, _ = self.srv.accept()
            except (socket.timeout, OSError):
                continue
            threading.Thread(target=self._wrap, args=(c,), daemon=True).start()

    def _wrap(self, c):
        try:
            self.handle(c)
        except OSError:
            pass
        finally:
            try:
                c.close()
            except OSError:
                pass

    @staticmethod
    def read_line(c, timeout=2.0):
        c.settimeout(timeout)
        buf = b""
        while b"\n" not in buf:
            chunk = c.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf.split(b"\n", 1)[0]

    def close(self):
        self.stop = True
        self.t.join()
        self.srv.close()
        try:
            os.unlink(self.path)
        except OSError:
            pass


WORKERS = {
    "w1": {},                                     # 명령 소켓: 기본 위치
    "w2": None,                                   # 명령 소켓: 설정 command_socket(setUpModule에서 채움)
    "w3": {},                                     # 명령 소켓 없음(꺼진 워커)
    "s1": {}, "s2": {}, "s3": {}, "q1": {}, "q2": {}, "slowr": {},  # 스트림 워커
}


class FakeManager(UnixServer):
    """Manager API 자리: 요청 한 줄 → 응답 한 줄. mode로 고장을 흉내 낸다."""

    def __init__(self, path):
        self.requests = []
        self.subs = 0      # 구독 시도(C++ 게이트웨이의 캐시 gw.mgr). 이 가짜는 구독을 모른다고 답해 캐시가 쓰이지 않는다
        self.mode = None   # None | "slow" | "garbage" | "close" | "huge"
        super().__init__(path)

    def handle(self, c):
        line = self.read_line(c)
        req = json.loads(line)
        if req.get("op") == "subscribe":  # requests에 넣지 않음(요청 모양 시험이 1초마다 오는 재시도와 섞이지 않게)
            self.subs += 1
            c.sendall(b'{"ok": false, "error": {"code": "bad_request", "message": "unknown op subscribe"}}\n')
            return
        self.requests.append(req)
        mode = self.mode
        if mode == "slow":
            time.sleep(3.0)
        if mode == "close":
            return
        if mode == "garbage":
            c.sendall(b"this is not json\n")
            return
        if mode == "huge":
            c.sendall(json.dumps({"ok": True, "data": {"x": "a" * 70000}}).encode() + b"\n")
            return
        c.sendall(json.dumps(self.reply(req), ensure_ascii=False).encode("utf-8") + b"\n")

    def reply(self, req):
        op, wid = req.get("op"), req.get("id")
        if op == "ping":
            return {"ok": True, "data": {"pong": True}}
        if op == "system":
            return {"ok": True, "data": {"uptime": 12.5, "이름": "보드"}}
        if op == "list":
            return {"ok": True, "data": {"workers": sorted(WORKERS)}}
        if isinstance(wid, str) and wid.startswith("e_"):
            code = wid[2:]
            return {"ok": False, "error": {"code": code, "message": "fake %s" % code, "fields": {"a": "b"}}}
        if op == "get" and wid == "nodata":
            return {"ok": True}
        if op in ("get", "get_config", "get_schema", "get_io") and wid not in WORKERS:
            return {"ok": False, "error": {"code": "not_found", "message": "unknown worker %s" % wid}}
        if op == "get_config":
            return {"ok": True, "data": {"config": WORKERS[wid] or {}, "preset": "default"}}
        return {"ok": True, "data": {"echo": req}}


class FakeCommandWorker(UnixServer):
    """워커 명령 소켓 자리"""

    def __init__(self, path, name):
        self.name = name
        self.requests = []
        super().__init__(path)

    def handle(self, c):
        req = json.loads(self.read_line(c))
        self.requests.append(req)
        cmd = req.get("cmd")
        if cmd == "slow":
            time.sleep(COMMAND_TIMEOUT * 3)
            reply = {"ok": True, "data": {"late": True}}
        elif cmd == "close":
            return
        elif cmd == "garbage":
            c.sendall(b"nope\n")
            return
        elif cmd == "nodata":
            reply = {"ok": True}
        elif cmd.startswith("err_"):
            reply = {"ok": False, "error": {"code": cmd[4:], "message": "worker says no"}}
        else:
            reply = {"ok": True, "data": {"worker": self.name, "cmd": cmd, "args": req.get("args")}}
        c.sendall(json.dumps(reply).encode() + b"\n")


class FakeStreamWorker:
    """워커 스트림 소켓 자리: 붙은 구독자에게 기준 상태를 먼저, send()로 변경분."""

    def __init__(self, path):
        self.path = path
        if os.path.exists(path):
            os.unlink(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.conns = []
        self.seq = 0
        self.state = {"vehicle": {"mode": "POSCTL", "roll": 0}}
        self.events = [{"seq": 1, "text": "device found"}]
        self.lock = threading.Lock()
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(8)
        self.srv.settimeout(0.1)
        self.stop = False
        self.accepted = 0
        self.t = threading.Thread(target=self._accept, daemon=True)
        self.t.start()

    def _line(self, msg):
        self.seq += 1
        msg["seq"] = self.seq
        msg["t"] = round(time.time(), 3)
        return (json.dumps(msg, ensure_ascii=False) + "\n").encode()

    def _accept(self):
        while not self.stop:
            try:
                c, _ = self.srv.accept()
            except (socket.timeout, OSError):
                continue
            with self.lock:
                self.accepted += 1
                c.sendall(self._line({"kf": True, "topics": dict(self.state), "events": list(self.events),
                                      "events_next": len(self.events) + 1}))
                self.conns.append(c)

    def send(self, topics=None, events=None):
        with self.lock:
            msg = {"kf": False, "topics": topics or {}}
            if topics:
                self.state.update(topics)
            if events:
                self.events.extend(events)
                msg["events"] = events
                msg["events_next"] = self.events[-1]["seq"] + 1
            data = self._line(msg)
            for c in list(self.conns):
                try:
                    c.settimeout(2.0)
                    c.sendall(data)
                except OSError:
                    self.conns.remove(c)

    def alive(self):
        """아직 열린(구독자가 닫지 않은) 연결 수"""
        n = 0
        with self.lock:
            for c in list(self.conns):
                try:
                    if c.recv(1, socket.MSG_DONTWAIT | socket.MSG_PEEK) == b"":
                        continue
                except BlockingIOError:
                    pass
                except OSError:
                    continue
                n += 1
        return n

    def close(self):
        self.stop = True
        self.t.join()
        with self.lock:
            for c in self.conns:
                c.close()
            self.conns = []
        self.srv.close()
        os.unlink(self.path)


class Server:
    """게이트웨이 프로세스(REST_BIN)"""

    def __init__(self, env=None, args=None, name="rest", unix_only=None):
        self.unix_only = unix_only  # TCP 없이 Unix 소켓만 여는 서버면 그 경로(뜬 것을 소켓 파일로 봄)
        self.port = int((env or {}).get("WORKSPACE_REST_PORT") or free_port())
        e = dict(os.environ)
        e.update({
            "WORKSPACE_DATA": T["data"], "WORKSPACE_ROOT": T["root"], "WORKSPACE_REST_BIND": "127.0.0.1",
            "WORKSPACE_REST_PORT": str(self.port), "WORKSPACE_REST_COMMAND_TIMEOUT": str(COMMAND_TIMEOUT),
            "WORKSPACE_LOG_LEVEL": "WARNING", "WORKSPACE_UI_TITLE": "Test WS", "PYTHONPATH": HERE,
        })
        for k in ("WORKSPACE_REST_TOKEN", "WORKSPACE_UI", "WORKSPACE_PRODUCT", "WORKSPACE_UI_DIR",
                  "WORKSPACE_MANAGER_SOCKET", "WORKSPACE_GATEWAY_SOCKET", "WORKSPACE_GATEWAY_THREADS", "WORKSPACE_GATEWAY_TCP",
                  "WORKSPACE_AUTH", "WORKSPACE_GATEWAY_CACHE"):
            e.pop(k, None)
        e.update(env or {})
        cmd = [REST_BIN]
        self.log_path = os.path.join(T["tmp"], "%s-%d.log" % (name, self.port))
        self.log = open(self.log_path, "wb")
        self.proc = subprocess.Popen(cmd + list(args or []), cwd=HERE, env=e, stdout=self.log, stderr=self.log)
        if not wait_for(self._up, START_TIMEOUT, 0.05):
            self.stop()
            raise RuntimeError("server did not start: %s" % open(self.log_path, errors="replace").read()[-2000:])

    def _up(self):
        if self.proc.poll() is not None:
            raise RuntimeError("server exited %s: %s" % (self.proc.returncode, open(self.log_path, errors="replace").read()[-2000:]))
        if self.unix_only:
            return os.path.exists(self.unix_only)
        try:
            socket.create_connection(("127.0.0.1", self.port), 0.2).close()
            return True
        except OSError:
            return False

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.log.close()

    def request(self, method, path, body=None, headers=None, timeout=10.0, raw_body=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        try:
            data = raw_body if raw_body is not None else (None if body is None else json.dumps(body).encode())
            conn.request(method, path, body=data, headers=headers or {})
            r = conn.getresponse()
            return r.status, {k.lower(): v for k, v in r.getheaders()}, r.read()
        finally:
            conn.close()

    def json(self, method, path, body=None, headers=None, **kw):
        status, hdrs, data = self.request(method, path, body, headers, **kw)
        return status, (json.loads(data) if data else None), hdrs


class SSE:
    """SSE 클라이언트(원시 소켓). 길이 없는 응답과 chunked 둘 다 읽는다."""

    def __init__(self, port, path, rcvbuf=None):
        self.s = socket.socket()
        if rcvbuf:
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.s.settimeout(5.0)
        self.s.connect(("127.0.0.1", port))
        self.s.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: text/event-stream\r\n\r\n" % path).encode())
        self.raw = b""
        self.body = b""
        self.eof = False
        while b"\r\n\r\n" not in self.raw:
            self._fill(5.0)
            if self.eof:
                raise OSError("closed before headers")
        head, self.raw = self.raw.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        self.status = int(lines[0].split()[1])
        self.headers = {}
        for line in lines[1:]:
            k, _, v = line.partition(":")
            self.headers[k.strip().lower()] = v.strip()
        self.chunked = self.headers.get("transfer-encoding", "").lower() == "chunked"
        self._decode()

    def _fill(self, timeout):
        self.s.settimeout(max(timeout, 0.01))
        try:
            data = self.s.recv(1 << 20)
        except socket.timeout:
            return
        except ConnectionResetError:
            data = b""
        if not data:
            self.eof = True
        self.raw += data

    def _decode(self):
        if not self.chunked:
            self.body += self.raw
            self.raw = b""
            return
        while True:
            if b"\r\n" not in self.raw:
                return
            size_line, rest = self.raw.split(b"\r\n", 1)
            size = int(size_line.split(b";")[0] or b"0", 16)
            if len(rest) < size + 2:
                return
            self.body += rest[:size]
            self.raw = rest[size + 2:]
            if size == 0:
                self.eof = True
                return

    def block(self, timeout=5.0):
        """다음 SSE 블록(빈 줄로 끝나는 줄들) 또는 None(시간 초과·끝)"""
        end = time.monotonic() + timeout
        while b"\n\n" not in self.body:
            left = end - time.monotonic()
            if left <= 0 or self.eof:
                return None
            self._fill(left)
            self._decode()
        blk, self.body = self.body.split(b"\n\n", 1)
        return blk.decode("utf-8")

    def msg(self, timeout=5.0):
        """다음 data 메시지(dict). ping·retry 블록은 건너뛴다. 없으면 None"""
        end = time.monotonic() + timeout
        while True:
            blk = self.block(max(0.0, end - time.monotonic()))
            if blk is None:
                return None
            if blk.startswith("data: "):
                return json.loads(blk[6:])

    def wait_msg(self, pred, timeout=5.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            m = self.msg(end - time.monotonic())
            if m is None:
                return None
            if pred(m):
                return m
        return None

    def close(self):
        self.s.close()


# ---- 임시 워크스페이스 ---------------------------------------------------------------------------

FAKE_BUILD = r'''import json, os, sys, time
a = sys.argv[1:]
if "--profiles" in a:
    print(json.dumps(["fail", "p1", "slow"]))
    sys.exit(0)
out = a[a.index("--out") + 1] if "--out" in a else "."
prof = a[0]
if "--check" in a:
    print("checking " + prof)
    print(json.dumps({"ok": prof != "bad", "profile": prof, "items": [{"key": "k", "label": "점검", "ok": True, "detail": "fine"}]}))
    sys.exit(0)
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "args.json"), "w") as f:
    json.dump(a, f)
print("##PROGRESS 10 준비", flush=True)
if prof == "slow":
    time.sleep(60)
    sys.exit(0)
if prof == "fail":
    print("##ERROR boom")
    sys.exit(3)
print("plain log line")
print("")
path = os.path.join(out, prof + "-bundle.tar.gz")
with open(path, "wb") as f:
    f.write(b"BUNDLE" * 1000)
pm = a[a.index("--power-mode") + 1] if "--power-mode" in a else None
print("##PROGRESS 100 끝")
print("##RESULT " + json.dumps({"file": os.path.basename(path), "power_mode": pm}))
'''


APP_JSON = '{\n  "password": "pw1",\n  "name": "x"\n}\n'


def iso_ago(minutes):
    return (datetime.datetime.now().astimezone() - datetime.timedelta(minutes=minutes)).isoformat(timespec="milliseconds")


def build_tree():
    tmp = tempfile.mkdtemp(prefix="ra")
    root, data = os.path.join(tmp, "ws"), os.path.join(tmp, "d")
    T.update(tmp=tmp, root=root, data=data)
    web = os.path.join(root, "ui", "web")
    write(os.path.join(web, "shell.html"), "<!doctype html><title>셸</title>\n")
    write(os.path.join(web, "kit.js"), "export const x = 1;\n")
    write(os.path.join(web, "shell.css"), "body{}\n")
    write(os.path.join(web, "data.json"), '{"a": 1}\n')
    write(os.path.join(web, "icon.svg"), "<svg/>\n")
    write(os.path.join(web, "img.png"), b"\x89PNG\r\n\x1a\nxxxx")
    write(os.path.join(web, "file.zzz"), b"\x00\x01")
    write(os.path.join(web, "sub", "page.html"), "<p>sub</p>")
    write(os.path.join(web, "panels", "board", "panel.json"), json.dumps({"title": "Board", "order": 1}))
    write(os.path.join(web, "panels", "board", "panel.js"), "// board\n")
    write(os.path.join(web, "panels", "logs", "panel.json"), json.dumps({"id": "ignored", "title": "Logs", "order": 5}))
    write(os.path.join(web, "panels", "bad", "panel.json"), "{not json")
    write(os.path.join(web, "panels", "nomanifest", "panel.js"), "//\n")
    wk = os.path.join(root, "workers")
    write(os.path.join(wk, "alpha", "workspace", "panel", "panel.json"), json.dumps({"title": "Alpha", "id": "alpha-panel", "order": 3, "worker_types": ["a"]}))
    write(os.path.join(wk, "alpha", "workspace", "panel", "panel.js"), "// alpha\n")
    write(os.path.join(wk, "beta", "workspace", "panel", "panel.json"), json.dumps({"title": "Beta"}))
    write(os.path.join(wk, "_template", "workspace", "panel", "panel.json"), json.dumps({"title": "T"}))
    write(os.path.join(wk, "dup", "workspace", "panel", "panel.json"), json.dumps({"id": "board", "title": "Dup"}))
    write(os.path.join(wk, "broken", "workspace", "panel", "panel.json"), "[1,")
    os.makedirs(os.path.join(wk, "nopanel"))
    os.makedirs(os.path.join(root, "logger"))
    shutil.copy(os.path.join(REPO, "logger", "workspace_history.py"), os.path.join(root, "logger"))
    write(os.path.join(root, "dev", "build_product.py"), FAKE_BUILD)

    # 워크스페이스 기본 설정 + 프로필 + 보드 설정
    cfgd = os.path.join(root, "install", "config")
    write(os.path.join(cfgd, "workspace.env"),
          "## 화면\n# 화면 제목\nWORKSPACE_UI_TITLE=Workspace\n\n# 포트\n# (기본 8080)\nWORKSPACE_REST_PORT=8080\n"
          "## 비밀\n# 토큰 예시\n# WORKSPACE_REST_TOKEN=abc\nAPI_KEY_X=defaultsecret\nPLAIN=1\n")
    write(os.path.join(cfgd, "app.json"), APP_JSON)
    write(os.path.join(cfgd, "same.conf"), "a=1\n")
    write(os.path.join(cfgd, "README.md"), "x")
    write(os.path.join(cfgd, "worker-types.json"), "{}")
    write(os.path.join(cfgd, ".hidden"), "x")
    prof = os.path.join(root, "install", "profiles", "p1")
    write(os.path.join(prof, "workspace.env"), "WORKSPACE_UI_TITLE=P1\nAPI_KEY_X=profsecret\n")
    write(os.path.join(prof, "app.json"), APP_JSON)
    write(os.path.join(prof, "extra.yaml"), "token: abc\r\nname: 이름\r\n  db_password = q\nnothing\n")
    write(os.path.join(prof, "product.json"), "{}")
    write(os.path.join(root, "install", "profiles", "p2", "app.json"), "{}")
    dcfg = os.path.join(data, "config")
    write(os.path.join(dcfg, "workspace.env"), "WORKSPACE_UI_TITLE=Board\nAPI_KEY_X=boardsecret\nEXTRA_ONLY=7\nMY_TOKEN=t\n")
    write(os.path.join(dcfg, "app.json"), APP_JSON)
    write(os.path.join(dcfg, "extra.yaml"), "token: abc\nname: 이름\n  db_password = changed\nnothing\n")
    write(os.path.join(dcfg, "same.conf"), "a=1\n")
    write(os.path.join(dcfg, "profile.json"), json.dumps({"name": "p1", "source": "install/profiles/p1"}))

    # 워커 로그
    lines = ["line %d 한글" % i for i in range(300)]
    write(os.path.join(data, "state", "logs", "w1.log"), ("\n".join(lines) + "\r\nCRLF end\n").encode() + b"bad \xff byte\n")
    # 기록: 파일 4개(명령은 마지막 3개에서만)
    rec = os.path.join(data, "records")
    def recl(t, typ, wid=None, metrics=None, **kw):
        d = {"t": t, "type": typ}
        if wid:
            d["id"] = wid
        if metrics is not None:
            d["metrics"] = metrics
        d.update(kw)
        return json.dumps(d, separators=(",", ":"), ensure_ascii=False)
    write(os.path.join(rec, "2026-01-01_aaaa.jsonl"), recl(iso_ago(5000), "command", op="old") + "\n")
    write(os.path.join(rec, "2026-01-02_bbbb.jsonl"), recl(iso_ago(3000), "command", op="b1") + "\n" +
          recl(iso_ago(52), "worker", "sysmon", {"cpu": 10, "mem": 1.5, "name": "x"}, resources={"cpu_pct": 3}, restart_count=1) + "\n")
    write(os.path.join(rec, "2026-01-03_cccc.jsonl"), "\n".join([
        recl(iso_ago(50), "worker", "sysmon", {"cpu": 20, "mem": 2.5, "flag": True}, resources={"cpu_pct": 5}, restart_count=2),
        recl(iso_ago(49), "worker", "other", {"cpu": 99}),
        recl(iso_ago(48), "system", "sysmon", {"cpu": 99}),
        "{broken json \"id\":\"sysmon\" \"type\":\"worker\"",
        recl(iso_ago(47), "command", op="c1"),
        recl(iso_ago(37), "worker", "sysmon", {"cpu": 30, "gpu": 1}),
        recl(iso_ago(35), "worker", "sysmon", {"cpu": 40, "gpu": 3, "도": 1}),
        recl(iso_ago(200), "worker", "sysmon", {"cpu": 1}),
    ]) + "\r\n")
    write(os.path.join(rec, "2026-01-04_dddd.jsonl"), "\n".join([
        recl(iso_ago(22), "worker", "sysmon", {"cpu": 50.25}),
        recl(iso_ago(7), "worker", "sysmon", {"cpu": 60, "mem": 3}, restart_count=3),
        recl(iso_ago(6), "command", op="d1", text="명령"),
        recl(iso_ago(5), "command", op="d2"),
    ]) + "\n")
    os.makedirs(os.path.join(data, "run", "cmd"), exist_ok=True)
    os.makedirs(os.path.join(data, "run", "stream"), exist_ok=True)


def setUpModule():
    build_tree()
    data = T["data"]
    T["mgr"] = FakeManager(os.path.join(data, "run", "manager-rest.sock"))
    custom = os.path.join(T["tmp"], "custom-w2.sock")
    WORKERS["w2"] = {"command_socket": custom}
    T["w1"] = FakeCommandWorker(os.path.join(data, "run", "cmd", "w1.sock"), "w1")
    T["w2"] = FakeCommandWorker(custom, "w2")
    T["srv"] = Server()


def tearDownModule():
    T["srv"].stop()
    for k in ("mgr", "w1", "w2"):
        T[k].close()
    shutil.rmtree(T["tmp"], ignore_errors=True)


def tail(path, n):
    """파일 끝 n줄(잘못된 UTF-8은 대체 문자, 줄 끝 CR 제거). 게이트웨이 로그 읽기의 기준"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        return []
    return data.decode("utf-8", "replace").splitlines()[-n:]


def oracle(module):
    """기준 코드: profile(reference_profile.py), history(logger의 workspace_history)"""
    os.environ["WORKSPACE_ROOT"] = T["root"]
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    if module == "history":
        sys.path.insert(0, os.path.join(T["root"], "logger"))
        try:
            import workspace_history
        finally:
            sys.path.pop(0)
        return workspace_history
    import reference_profile
    return reference_profile


# ---- 신분 확인 도우미(C++ 게이트웨이 전용, docs/gateway.md 3장) ---------------------------------------

PASSWORDS = {"v1": "viewer-pass-1", "o1": "operator-pass-1", "m1": "maint-pass-1"}
ROLES = {"v1": "viewer", "o1": "operator", "m1": "maintainer"}
AUTH_USERS_TOKEN = {}  # 데이터 폴더 → m1의 토큰 "ci"(원문, 시험만 앎)


def slurp(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def gw_cli(data, *args, stdin=None):
    """게이트웨이 설정 명령(gateway user …/token …). 반복 수는 시험이 빨리 돌게 줄인다"""
    env = dict(os.environ, WORKSPACE_DATA=data, WORKSPACE_AUTH_ITERATIONS="1000")
    return subprocess.run([REST_BIN] + list(args), input=stdin, capture_output=True, text=True, env=env, timeout=60)


def add_user(data, name, role, password):
    r = gw_cli(data, "user", "add", name, role, stdin="%s\n%s\n" % (password, password))
    assert r.returncode == 0, r.stderr
    return r


def auth_data_dir():
    """임시 데이터 폴더 + 사용자 v1·o1·m1 + m1의 토큰 ci"""
    data = tempfile.mkdtemp(prefix="auth", dir=T["tmp"])
    for name, pw in PASSWORDS.items():
        add_user(data, name, ROLES[name], pw)
    r = gw_cli(data, "token", "add", "m1", "ci")
    assert r.returncode == 0, r.stderr
    AUTH_USERS_TOKEN[data] = r.stdout.strip()
    return data


def auth_env(data, **extra):
    env = {"WORKSPACE_DATA": data, "WORKSPACE_MANAGER_SOCKET": T["mgr"].path, "WORKSPACE_AUTH": "on"}
    env.update(extra)
    return env


def users_count(srv):
    """게이트웨이가 지금 들고 있는 사용자 수(/health?detail=1)"""
    return srv.json("GET", "/health?detail=1")[1]["gateway"]["users"]["users"]


def log_lines(data):
    out = []
    d = os.path.join(data, "records", "gateway")
    for name in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        with open(os.path.join(d, name), encoding="utf-8") as f:
            out += [json.loads(line) for line in f if line.strip()]
    return out


def log_text(data):
    d = os.path.join(data, "records", "gateway")
    text = ""
    for name in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        with open(os.path.join(d, name), encoding="utf-8") as f:
            text += f.read()
    return text


class Session:
    """한 사용자(쿠키 세션 또는 토큰)로 요청"""

    def __init__(self, srv, token=None):
        self.srv, self.cookie, self.token = srv, None, token

    def headers(self, extra=None):
        h = dict(extra or {})
        if self.cookie:
            h["Cookie"] = "workspace_session=" + self.cookie
        if self.token:
            h["Authorization"] = "Bearer " + self.token
        return h

    def json(self, method, path, body=None, headers=None):
        return self.srv.json(method, path, body, headers=self.headers(headers))

    def login(self, name, password):
        st, hdrs, data = self.srv.request("POST", "/api/auth/login", {"name": name, "password": password})
        if st == 200:
            cookie = hdrs["set-cookie"]
            self.cookie = cookie.split(";", 1)[0].split("=", 1)[1]
        return st, (json.loads(data) if data else None), hdrs


class UnixHTTPConnection(http.client.HTTPConnection):
    def __init__(self, path, timeout=10):
        super().__init__("localhost", timeout=timeout)
        self.unix_path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.unix_path)


def unix_json(path, method, target, body=None, headers=None):
    conn = UnixHTTPConnection(path)
    try:
        conn.request(method, target, body=None if body is None else json.dumps(body).encode(), headers=headers or {})
        r = conn.getresponse()
        data = r.read()
        return r.status, (json.loads(data) if data else None)
    finally:
        conn.close()


class SubManager(UnixServer):
    """구독을 아는 가짜 Manager(캐시 시험): 0.3 s마다 워커 전체 + system. stall이면 구독은 조용하고 요청은 3 s 늦음"""

    def __init__(self, path):
        self.workers = [{"id": "w2", "actual_state": "running", "metrics": {"도": 1.5}, "pid": 12},
                        {"id": "w1", "actual_state": "stopped", "metrics": {}, "pid": None}]
        self.system = {"clients": {"rest": "subscribed"}, "access_mode": "operation", "이름": "보드"}
        self.stall = False
        self.ops = []
        self.lock = threading.Lock()
        super().__init__(path)

    def count(self, op):
        with self.lock:
            return sum(1 for r in self.ops if r.get("op") == op)

    def handle(self, c):
        req = json.loads(self.read_line(c))
        op = req.get("op")
        with self.lock:
            self.ops.append(req)
        send = lambda obj: c.sendall(json.dumps(obj, ensure_ascii=False).encode("utf-8") + b"\n")
        if op == "subscribe":
            send({"ok": True, "data": {"subscribed": ["system", "workers"]}})
            while not self.stop:
                if not self.stall:
                    for w in self.workers:
                        send({"event": "worker_status", "data": w})
                    send({"event": "system", "data": self.system})
                time.sleep(0.3)
            return
        if self.stall:
            time.sleep(3.0)
        if op == "list":
            send({"ok": True, "data": self.workers})
        elif op == "get":
            hit = [w for w in self.workers if w["id"] == req.get("id")]
            send({"ok": True, "data": hit[0]} if hit else {"ok": False, "error": {"code": "not_found", "message": "unknown"}})
        elif op == "system":
            send({"ok": True, "data": self.system})
        elif op == "set_access_mode":
            send({"ok": True, "data": {"access_mode": req.get("mode")}})
        else:
            send({"ok": True, "data": {"pong": True}})


# ---- 시험 --------------------------------------------------------------------------------------

class ManagerRoutes(unittest.TestCase):
    """Manager op 번역: 경로·메서드·상태 코드·요청 모양"""

    def setUp(self):
        self.srv, self.mgr = T["srv"], T["mgr"]
        self.mgr.mode = None

    def call(self, method, path, body=None, **kw):
        n = len(self.mgr.requests)
        status, obj, hdrs = self.srv.json(method, path, body, **kw)
        req = self.mgr.requests[-1] if len(self.mgr.requests) > n else None
        return status, obj, req, hdrs

    def test_route_table(self):
        table = [
            ("GET", "/system", None, 200, {"op": "system"}),
            ("GET", "/workers", None, 200, {"op": "list"}),
            ("GET", "/workers/w1", None, 200, {"op": "get", "id": "w1"}),
            ("GET", "/workers/w1/config", None, 200, {"op": "get_config", "id": "w1"}),
            ("PUT", "/workers/w1/config", {"a": 1}, 202, {"op": "set_config", "id": "w1", "config": {"a": 1}}),
            ("POST", "/workers/w1/config/apply", {"b": 2}, 202, {"save": False, "op": "set_config", "id": "w1", "config": {"b": 2}}),
            ("POST", "/workers/w1/presets", {"name": "x"}, 202, {"op": "save_preset", "id": "w1", "preset": {"name": "x"}}),
            ("POST", "/workers/w1/presets/p-1_a/use", None, 202, {"op": "use_preset", "id": "w1", "name": "p-1_a"}),
            ("PUT", "/workers/w1/presets/p-1", {"name": "p2"}, 202, {"op": "rename_preset", "id": "w1", "name": "p-1", "preset": {"name": "p2"}}),
            ("DELETE", "/workers/w1/presets/p-1", None, 202, {"op": "delete_preset", "id": "w1", "name": "p-1"}),
            ("GET", "/workers/w1/schema", None, 200, {"op": "get_schema", "id": "w1"}),
            ("GET", "/workers/w1/io", None, 200, {"op": "get_io", "id": "w1"}),
            ("PUT", "/workers/w1/autostart", {"autostart": True}, 202, {"op": "set_autostart", "id": "w1", "autostart": {"autostart": True}}),
            ("POST", "/workers/w1/start", None, 202, {"op": "start", "id": "w1"}),
            ("POST", "/workers/w1/stop", None, 202, {"op": "stop", "id": "w1"}),
        ]
        for method, path, body, code, expect in table:
            for prefix in ("", "/api/board"):
                for suffix in ("", "/"):
                    with self.subTest(method=method, path=prefix + path + suffix):
                        status, obj, req, hdrs = self.call(method, prefix + path + suffix, body)
                        self.assertEqual(status, code)
                        self.assertEqual(req, expect)
                        self.assertEqual(hdrs["content-type"], "application/json; charset=utf-8")
                        if expect["op"] == "system":
                            self.assertEqual(obj, {"uptime": 12.5, "이름": "보드"})
                        elif expect["op"] == "list":
                            self.assertEqual(obj, {"workers": sorted(WORKERS)})
                        elif expect["op"] == "get_config":
                            self.assertEqual(obj, {"config": {}, "preset": "default"})
                        else:
                            self.assertEqual(obj, {"echo": expect})

    def test_body_variants(self):
        # 본문 없음 → None, 잘못된 JSON → 400, 크기 초과 → 400
        status, obj, req, _ = self.call("PUT", "/workers/w1/config")
        self.assertEqual((status, req), (202, {"op": "set_config", "id": "w1", "config": None}))
        status, obj, req, _ = self.call("PUT", "/workers/w1/config", raw_body=b"{bad")
        self.assertEqual(status, 400)
        self.assertEqual(obj["error"]["code"], "bad_request")
        self.assertIsNone(req)
        # 크기 초과: 본문을 읽지 않고 400(본문은 보내지 않아 RST 없이 답을 받는다)
        st, body = raw_request(self.srv.port, CRLF.join([b"PUT /workers/w1/config HTTP/1.1", b"Host: x",
                                                          b"Content-Length: 70000", b"", b""]))
        self.assertEqual(st, 400)
        self.assertEqual(json.loads(body)["error"], {"code": "bad_request", "message": "invalid JSON body: body too large"})
        st, body = raw_request(self.srv.port, CRLF.join([b"POST /workers/w1/command HTTP/1.1", b"Host: x",
                                                          b"Content-Length: 65537", b"", b""]))
        self.assertEqual(st, 400)
        # 한글·배열 본문 그대로
        status, obj, req, _ = self.call("POST", "/workers/w1/presets", ["가", 1, None])
        self.assertEqual(req["preset"], ["가", 1, None])

    def test_errors(self):
        for code, status in (("bad_request", 400), ("forbidden", 403), ("not_found", 404), ("conflict", 409),
                             ("internal", 500), ("whatever", 500)):
            with self.subTest(code=code):
                st, obj, req, _ = self.call("POST", "/workers/e_%s/start" % code)
                self.assertEqual(st, status)
                self.assertEqual(obj, {"error": {"code": code, "message": "fake %s" % code, "fields": {"a": "b"}}})
        st, obj, _, _ = self.call("GET", "/workers/nope")
        self.assertEqual((st, obj["error"]["code"]), (404, "not_found"))
        st, obj, _, _ = self.call("GET", "/workers/nodata")
        self.assertEqual((st, obj), (200, None))

    def test_method_and_route_errors(self):
        for method, path in (("DELETE", "/workers/w1"), ("GET", "/workers/w1/start"), ("POST", "/workers"),
                             ("PUT", "/system"), ("GET", "/workers/w1/command"), ("POST", "/workers/w1/stream"),
                             ("DELETE", "/workers/w1/config/apply"), ("POST", "/api/board/workers/w1")):
            with self.subTest(method=method, path=path):
                st, obj, req, _ = self.call(method, path)
                self.assertEqual(st, 405)
                self.assertEqual(obj["error"], {"code": "method_not_allowed",
                                                "message": "%s not allowed on %s" % (method, path.replace("/api/board", ""))})
                self.assertIsNone(req)
        for method, path in (("GET", "/nope"), ("POST", "/workers/w1/presets/BAD/use"), ("PUT", "/workers/w1/presets/a/b"),
                             ("GET", "/workers/w1/zzz"), ("POST", "/health"), ("POST", "/logs/service"),
                             ("POST", "/api/panels"), ("GET", "/api/board/nope"), ("GET", "/workers/w1/presets/x/use/more")):
            with self.subTest(method=method, path=path):
                st, obj, req, _ = self.call(method, path)
                self.assertEqual(st, 404)
                self.assertEqual(obj["error"], {"code": "not_found", "message": "no route %s" % path.replace("/api/board", "")})

    def test_manager_faults(self):
        for mode in ("garbage", "close", "huge", "slow"):
            with self.subTest(mode=mode):
                self.mgr.mode = mode
                t0 = time.monotonic()
                st, obj, _, _ = self.call("GET", "/workers")
                self.assertEqual(st, 503)
                self.assertEqual(obj["error"]["code"], "unavailable")
                self.assertTrue(obj["error"]["message"].startswith("manager unavailable: "))
                if mode == "slow":
                    self.assertLess(time.monotonic() - t0, 2.9)
                    self.assertGreater(time.monotonic() - t0, 1.5)
        self.mgr.mode = None

    def test_health(self):
        st, obj, _, _ = self.call("GET", "/health")
        self.assertEqual((st, obj), (200, {"rest": "ok", "manager": "ok"}))
        st, obj, _, _ = self.call("GET", "/api/board/health/")
        self.assertEqual((st, obj), (200, {"rest": "ok", "manager": "ok"}))


class Commands(unittest.TestCase):
    """POST /workers/{id}/command → 워커 명령 소켓"""

    def setUp(self):
        self.srv = T["srv"]
        T["mgr"].mode = None

    def test_ok_default_and_custom_socket(self):
        st, obj, _ = self.srv.json("POST", "/workers/w1/command", {"cmd": "source.set_rate", "args": {"hz": 20}})
        self.assertEqual((st, obj), (200, {"worker": "w1", "cmd": "source.set_rate", "args": {"hz": 20}}))
        self.assertEqual(T["w1"].requests[-1], {"cmd": "source.set_rate", "args": {"hz": 20}})
        st, obj, _ = self.srv.json("POST", "/api/board/workers/w2/command/", {"cmd": "x"})
        self.assertEqual((st, obj), (200, {"worker": "w2", "cmd": "x", "args": {}}))
        self.assertEqual(T["w2"].requests[-1], {"cmd": "x", "args": {}})
        st, obj, _ = self.srv.json("POST", "/workers/w1/command", {"cmd": "nodata", "args": None})
        self.assertEqual((st, obj), (200, None))

    def test_worker_errors(self):
        for code, status in (("bad_request", 400), ("unavailable", 503), ("timeout", 504), ("internal", 500),
                             ("busy", 500), ("forbidden", 500)):
            with self.subTest(code=code):
                st, obj, _ = self.srv.json("POST", "/workers/w1/command", {"cmd": "err_" + code})
                self.assertEqual(st, status)
                self.assertEqual(obj, {"error": {"code": code, "message": "worker says no"}})

    def test_transport_faults(self):
        st, obj, _ = self.srv.json("POST", "/workers/w3/command", {"cmd": "x"})
        self.assertEqual(st, 503)
        self.assertEqual(obj["error"], {"code": "unavailable", "message": "worker w3 is not running (no command socket)"})
        t0 = time.monotonic()
        st, obj, _ = self.srv.json("POST", "/workers/w1/command", {"cmd": "slow"})
        self.assertEqual(st, 504)
        self.assertEqual(obj["error"], {"code": "timeout", "message": "worker w1 did not answer"})
        self.assertLess(time.monotonic() - t0, COMMAND_TIMEOUT + 1.0)
        for cmd in ("close", "garbage"):
            st, obj, _ = self.srv.json("POST", "/workers/w1/command", {"cmd": cmd})
            self.assertEqual(st, 503)
            self.assertEqual(obj["error"]["code"], "unavailable")
            self.assertTrue(obj["error"]["message"].startswith("worker w1: "))
        # 소켓 파일은 있는데 아무도 안 받음
        dead = os.path.join(T["data"], "run", "cmd", "dead.sock")
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(dead)
        s.close()
        WORKERS["dead"] = {}
        try:
            st, obj, _ = self.srv.json("POST", "/workers/dead/command", {"cmd": "x"})
            self.assertEqual(st, 503)
            self.assertTrue(obj["error"]["message"].startswith("worker dead: "))
        finally:
            del WORKERS["dead"]
            os.unlink(dead)

    def test_bad_requests(self):
        for body in (b"[1]", b'{"args": {}}', b'{"cmd": 3}', b"{bad", b""):
            with self.subTest(body=body):
                st, obj, _ = self.srv.json("POST", "/workers/w1/command", raw_body=body)
                self.assertEqual(st, 400)
                self.assertEqual(obj["error"]["code"], "bad_request")
        st, obj, _ = self.srv.json("POST", "/workers/nope/command", {"cmd": "x"})
        self.assertEqual((st, obj["error"]["code"]), (404, "not_found"))
        st, obj, _ = self.srv.json("POST", "/workers/e_forbidden/command", {"cmd": "x"})
        self.assertEqual(st, 403)


class Ui(unittest.TestCase):
    """화면 제공: 파일, /api/panels, /api/ui, /api/remote"""

    def setUp(self):
        self.srv = T["srv"]

    def test_static_files(self):
        cases = [
            ("/", "shell.html", "text/html; charset=utf-8"),
            ("/index.html", "shell.html", "text/html; charset=utf-8"),
            ("/kit.js", "kit.js", "text/javascript; charset=utf-8"),
            ("/shell.css", "shell.css", "text/css; charset=utf-8"),
            ("/data.json", "data.json", "application/json; charset=utf-8"),
            ("/icon.svg", "icon.svg", "image/svg+xml; charset=utf-8"),
            ("/img.png", "img.png", "image/png"),
            ("/file.zzz", "file.zzz", "application/octet-stream"),
            ("/sub/page.html", "sub/page.html", "text/html; charset=utf-8"),
            ("/sub/./x/../page.html?v=3", "sub/page.html", "text/html; charset=utf-8"),
            ("/panels/board/panel.js", "panels/board/panel.js", "text/javascript; charset=utf-8"),
        ]
        web = os.path.join(T["root"], "ui", "web")
        for path, rel, ctype in cases:
            with self.subTest(path=path):
                st, hdrs, data = self.srv.request("GET", path)
                self.assertEqual(st, 200)
                self.assertEqual(hdrs["content-type"], ctype)
                self.assertEqual(hdrs.get("cache-control"), "no-store")
                with open(os.path.join(web, rel), "rb") as f:
                    self.assertEqual(data, f.read())
        wk = os.path.join(T["root"], "workers")
        for path, file in (("/panels/alpha-panel/panel.js", os.path.join(wk, "alpha", "workspace", "panel", "panel.js")),
                           ("/panels/beta/panel.json", os.path.join(wk, "beta", "workspace", "panel", "panel.json"))):
            st, hdrs, data = self.srv.request("GET", path)
            self.assertEqual(st, 200)
            with open(file, "rb") as f:
                self.assertEqual(data, f.read())

    def test_path_traversal(self):
        for path in ("/../../etc/passwd", "/sub/../../ws/logger/workspace_history.py", "/%2e%2e/%2e%2e/etc/passwd",
                     "/panels/alpha-panel/../../../../../etc/passwd", "/panels/alpha-panel//etc/passwd",
                     "/panels/nosuch/panel.js", "/panels/", "/sub", "/..", "/panels/_template/panel.json"):
            with self.subTest(path=path):
                st, hdrs, data = self.srv.request("GET", path)
                self.assertEqual(st, 404)
                self.assertEqual(json.loads(data)["error"]["code"], "not_found")

    def test_panels(self):
        st, obj, _ = self.srv.json("GET", "/api/panels")
        self.assertEqual(st, 200)
        self.assertEqual(obj, [
            {"title": "Board", "order": 1, "id": "board"},
            {"title": "Alpha", "id": "alpha-panel", "order": 3, "worker_types": ["a"]},
            {"id": "logs", "title": "Logs", "order": 5},
            {"title": "Beta", "id": "beta"},
        ])
        self.assertEqual([list(p) for p in obj][2], ["id", "title", "order"])  # 키 순서도 그대로
        st, obj2, _ = self.srv.json("GET", "/api/board/../api/panels")
        self.assertEqual(st, 404)

    def test_ui_info(self):
        for host, media in (("Example.COM:8080", "http://example.com:8889/{path}"), ("[::1]:8080", "http://[::1]:8889/{path}"),
                            ("board.test", "http://board.test:8889/{path}"),("user@h:1", "http://h:8889/{path}")):
            with self.subTest(host=host):
                st, obj, _ = self.srv.json("GET", "/api/ui", headers={"Host": host})
                self.assertEqual(st, 200)
                self.assertEqual(obj["media"], {"url": media})
                self.assertEqual(obj["title"], "Test WS")
                self.assertIs(obj["product"], False)
                self.assertEqual(obj["refresh_ms"], 1000)
                self.assertEqual(obj["board"], {"data_dir": T["data"]})
                self.assertIsNone(obj["rest_last_error"])
                self.assertLess(abs(obj["rest_last_ok"] - time.time()), 5)
                self.assertEqual(list(obj), ["title", "product", "refresh_ms", "media", "board", "rest_last_ok", "rest_last_error"])
        # Host 헤더 없음 → 127.0.0.1
        conn = http.client.HTTPConnection("127.0.0.1", self.srv.port, timeout=5)
        conn.putrequest("GET", "/api/ui", skip_host=True)
        conn.endheaders()
        obj = json.loads(conn.getresponse().read())
        conn.close()
        self.assertEqual(obj["media"]["url"], "http://127.0.0.1:8889/{path}")

    def test_remote(self):
        log = os.path.join(T["data"], "state", "logs", "w1.log")
        st, obj, _ = self.srv.json("GET", "/api/remote/worker_log?id=w1&lines=4")
        self.assertEqual((st, obj), (200, {"ok": True, "lines": tail(log, 4)}))
        st, obj, _ = self.srv.json("GET", "/api/remote/worker_log?id=w1")
        self.assertEqual(obj["lines"], tail(log, 200))
        st, obj, _ = self.srv.json("GET", "/api/remote/commands?lines=2")
        self.assertEqual(obj["ok"], True)
        self.assertEqual([json.loads(x)["op"] for x in obj["lines"]], ["d1", "d2"])
        st, obj, _ = self.srv.json("GET", "/api/remote/processes?lines=3")
        self.assertTrue(obj["ok"])
        self.assertLessEqual(len(obj["lines"]), 3)
        for line in obj["lines"]:
            parts = line.split(" ")
            self.assertEqual(len(parts), 5)
            int(parts[0]), float(parts[2]), float(parts[3]), int(parts[4])
            self.assertRegex(parts[2], r"^\d+\.\d$")
        st, obj, _ = self.srv.json("GET", "/api/remote/power_modes")
        self.assertEqual(st, 200)
        self.assertTrue(obj["ok"])
        st, obj, _ = self.srv.json("GET", "/api/remote/service_log?lines=3")
        self.assertEqual(st, 200)
        self.assertIn("ok", obj)
        st, obj, _ = self.srv.json("GET", "/api/remote/metrics_history?id=sysmon&points=0&keys=cpu&csv=1")
        self.assertTrue(obj["ok"])
        self.assertEqual(obj["lines"][0], "time,cpu")
        self.assertEqual(len(obj["lines"]), 7)
        for path, st_, body in (("/api/remote/nope", 404, {"error": "unknown remote command"}),
                                ("/api/remote/workspace", 404, {"error": "unknown remote command"}),
                                ("/api/remote/worker_log", 400, {"error": "bad worker id"}),
                                ("/api/remote/worker_log?id=-x", 400, {"error": "bad worker id"}),
                                ("/api/remote/worker_log?id=a%2Fb", 400, {"error": "bad worker id"}),
                                ("/api/remote/worker_log?id=w1&lines=abc", 400, {"error": "lines must be an integer"}),
                                ("/api/remote/metrics_history?id=s&keys=A", 400, {"error": "bad keys"})):
            with self.subTest(path=path):
                st, obj, _ = self.srv.json("GET", path)
                self.assertEqual((st, obj), (st_, body))
        st, obj, _ = self.srv.json("POST", "/api/remote/worker_log?id=w1")
        self.assertEqual(st, 404)


class Reads(unittest.TestCase):
    """읽기 전용 보드 조회(reads.py)"""

    def setUp(self):
        self.srv = T["srv"]

    def test_worker_log(self):
        log = os.path.join(T["data"], "state", "logs", "w1.log")
        for q, n in (("", 200), ("?lines=1", 1), ("?lines=0", 1), ("?lines=3", 3), ("?lines=99999", 5000),
                     ("?lines=", 200), ("?lines=7&lines=2", 2)):
            with self.subTest(q=q):
                st, obj, _ = self.srv.json("GET", "/logs/workers/w1" + q)
                self.assertEqual((st, obj), (200, {"lines": tail(log, n)}))
        self.assertEqual(self.srv.json("GET", "/logs/workers/w1?lines=2")[1]["lines"], ["CRLF end", "bad \ufffd byte"])
        st, obj, _ = self.srv.json("GET", "/logs/workers/none/")
        self.assertEqual((st, obj), (200, {"lines": []}))
        st, obj, _ = self.srv.json("GET", "/logs/workers/-bad")
        self.assertEqual((st, obj), (400, {"error": {"code": "bad_request", "message": "bad worker id"}}))
        st, obj, _ = self.srv.json("GET", "/logs/workers/w1?lines=x")
        self.assertEqual((st, obj), (400, {"error": {"code": "bad_request", "message": "lines must be an integer"}}))

    def test_commands(self):
        st, obj, _ = self.srv.json("GET", "/records/commands")
        self.assertEqual(st, 200)
        self.assertEqual([json.loads(x)["op"] for x in obj["lines"]], ["b1", "c1", "d1", "d2"])
        self.assertFalse(any(x.endswith("\r") for x in obj["lines"]))
        st, obj, _ = self.srv.json("GET", "/api/board/records/commands?lines=1")
        self.assertEqual([json.loads(x)["op"] for x in obj["lines"]], ["d2"])

    def test_processes_and_power(self):
        st, obj, _ = self.srv.json("GET", "/system/processes?lines=5")
        self.assertEqual(st, 200)
        self.assertTrue(1 <= len(obj["processes"]) <= 5)
        for p in obj["processes"]:
            self.assertEqual(list(p), ["pid", "name", "cpu", "mem", "rss_kb"])
            self.assertIsInstance(p["pid"], int)
            self.assertIsInstance(p["cpu"], float)
            self.assertIsInstance(p["rss_kb"], int)
        st, obj, _ = self.srv.json("GET", "/system/power_modes")
        self.assertEqual(st, 200)
        self.assertIsInstance(obj["modes"], dict)
        st, obj, _ = self.srv.json("GET", "/logs/service?lines=2")
        self.assertIn(st, (200, 503))
        if st == 200:
            self.assertIsInstance(obj["lines"], list)
        else:
            self.assertEqual(obj["error"]["code"], "unavailable")

    def test_worker_tasks(self):
        p = subprocess.Popen(["sleep", "30"], start_new_session=True)
        try:
            time.sleep(0.1)
            st, obj, _ = self.srv.json("GET", "/workers/anything/tasks?pid=%d" % p.pid)
            self.assertEqual(st, 200)
            self.assertEqual(obj["pid"], p.pid)
            self.assertEqual(len(obj["processes"]), 1)
            pr = obj["processes"][0]
            self.assertEqual(list(pr), ["pid", "ppid", "name", "rss_mb", "cpu", "thread_count", "threads"])
            self.assertEqual((pr["pid"], pr["name"], pr["thread_count"]), (p.pid, "sleep", 1))
            th = pr["threads"][0]
            self.assertEqual(list(th), ["tid", "name", "cpu", "policy", "rt_priority", "nice", "cpus"])
            self.assertEqual((th["tid"], th["name"], th["cpu"], th["policy"], th["rt_priority"]), (p.pid, "sleep", 0.0, "normal", 0))
            self.assertIsInstance(th["cpus"], str)
        finally:
            p.kill()
            p.wait()
        for q in ("", "?pid=1", "?pid=abc"):
            st, obj, _ = self.srv.json("GET", "/workers/w1/tasks" + q)
            self.assertEqual(st, 400)
        st, obj, _ = self.srv.json("GET", "/workers/w1/tasks?pid=999999")
        self.assertEqual((st, obj), (200, {"pid": 999999, "processes": []}))

    def test_workspace_info(self):
        profile = oracle("profile")
        expect = profile.info(T["root"], T["data"])
        st, obj, _ = self.srv.json("GET", "/system/workspace")
        self.assertEqual(st, 200)
        self.assertEqual(obj, json.loads(json.dumps(expect)))
        self.assertEqual(list(obj), list(expect))
        self.assertEqual([list(e) for e in obj["env"]], [list(e) for e in expect["env"]])
        # 비밀값 가림 확인(정답과 별개로)
        files = {f["name"]: f for f in obj["files"]}
        self.assertIn('"password": ••••', files["app.json"]["text"])
        self.assertEqual(files["extra.yaml"]["text"], "token: ••••\nname: 이름\n  db_password = ••••\nnothing\n")
        self.assertEqual(files["extra.yaml"]["origin"], "changed")
        self.assertEqual(files["app.json"]["origin"], "profile")
        self.assertEqual(files["same.conf"]["origin"], "default")
        env = {e["key"]: e for e in obj["env"]}
        self.assertEqual(env["API_KEY_X"]["value"], "••••")
        self.assertEqual(env["API_KEY_X"]["default"], "••••")
        self.assertEqual(env["MY_TOKEN"]["value"], "••••")
        self.assertEqual(env["WORKSPACE_REST_TOKEN"]["example"], "abc")

    def test_workspace_guess_profile(self):
        # profile.json이 없으면 가장 많이 같은 프로필을 추정
        pj = os.path.join(T["data"], "config", "profile.json")
        os.rename(pj, pj + ".off")
        try:
            profile = oracle("profile")
            expect = profile.info(T["root"], T["data"])
            st, obj, _ = self.srv.json("GET", "/system/workspace")
            self.assertEqual(obj, json.loads(json.dumps(expect)))
            self.assertEqual(obj["profile"]["name"], "p1")
            self.assertTrue(obj["profile"]["guessed"])
        finally:
            os.rename(pj + ".off", pj)

    def test_history(self):
        hist = oracle("history")
        recdir = os.path.join(T["data"], "records")
        for q, keys, csv in (("points=0", None, False), ("points=0&keys=cpu,res.cpu_pct,zz", ["cpu", "res.cpu_pct", "zz"], False),
                             ("points=0&csv=1", None, True), ("points=0&keys=mem,cpu&csv=1", ["mem", "cpu"], True),
                             ("minutes=30&points=0", None, False)):
            with self.subTest(q=q):
                minutes = 30 if "minutes=30" in q else 60
                expect = hist.render(hist.query(recdir, "sysmon", minutes, 0, keys), keys, csv)
                st, obj, _ = self.srv.json("GET", "/history/sysmon?" + q)
                self.assertEqual((st, obj), (200, {"lines": expect}))
        # 칸 평균(칸 경계는 지금 시각에 따라 조금 움직이므로 값·개수만 정확히, 시각은 근사)
        expect = hist.query(recdir, "sysmon", 60, 4, None)
        st, obj, _ = self.srv.json("GET", "/history/sysmon?minutes=60&points=4")
        got = [json.loads(x) for x in obj["lines"]]
        self.assertEqual([g["m"] for g in got], [m for _, m in expect])
        self.assertEqual([list(g["m"]) for g in got], [list(m) for _, m in expect])
        for g, (t, _) in zip(got, expect):
            self.assertLess(abs(datetime.datetime.fromisoformat(g["t"]).timestamp() - t), 5)
        self.assertEqual(got[0]["m"]["cpu"], 15.0)
        st, obj, _ = self.srv.json("GET", "/history/nobody")
        self.assertEqual((st, obj), (200, {"lines": []}))
        st, obj, _ = self.srv.json("GET", "/history/nobody?csv=1")
        self.assertEqual((st, obj), (200, {"lines": ["time"]}))
        for q in ("minutes=x", "points=y", "keys=A"):
            st, obj, _ = self.srv.json("GET", "/history/sysmon?" + q)
            self.assertEqual(st, 400)


class Streams(unittest.TestCase):
    """GET /workers/{id}/stream: 워커 스트림 → SSE"""

    def setUp(self):
        self.srv = T["srv"]
        T["mgr"].mode = None
        self.sse = []
        self.workers = []

    def tearDown(self):
        for s in self.sse:
            s.close()
        for w in self.workers:
            try:
                w.close()
            except OSError:
                pass

    def worker(self, wid):
        w = FakeStreamWorker(os.path.join(T["data"], "run", "stream", wid + ".sock"))
        self.workers.append(w)
        return w

    def open(self, wid, hz=None, **kw):
        s = SSE(self.srv.port, "/workers/%s/stream%s" % (wid, "" if hz is None else "?hz=%s" % hz), **kw)
        self.sse.append(s)
        return s

    def test_keyframe_merge_restart(self):
        w = self.worker("s1")
        a = self.open("s1", hz=20)
        self.assertEqual(a.status, 200)
        self.assertEqual(a.headers["content-type"], "text/event-stream; charset=utf-8")
        self.assertEqual(a.headers["cache-control"], "no-store")
        self.assertEqual(a.headers["x-accel-buffering"], "no")
        self.assertEqual(a.block(), "retry: 2000")
        first = a.msg()
        self.assertTrue(first["kf"])
        self.assertEqual(first["seq"], 1)
        kf = first if first.get("stream") == "up" else a.wait_msg(lambda m: m.get("stream") == "up")
        self.assertTrue(kf["kf"])
        self.assertEqual(kf["topics"], {"vehicle": {"mode": "POSCTL", "roll": 0}})
        self.assertEqual(kf["events"], [{"seq": 1, "text": "device found"}])
        self.assertEqual(kf["events_next"], 2)
        self.assertIn("t", kf)
        self.assertEqual(list(kf)[:3], ["seq", "kf", "topics"])

        # 변경분: 주제는 최신 값, 이벤트는 모두
        w.send({"vehicle": {"mode": "POSCTL", "roll": 1}})
        w.send({"vehicle": {"mode": "HOLD", "roll": 2}}, [{"seq": 2, "text": "POSCTL -> HOLD"}])
        w.send({"link": {"loss_pct": 1.5}})
        topics, events = {}, []
        while "link" not in topics:
            m = a.msg(3.0)
            self.assertIsNotNone(m)
            self.assertFalse(m["kf"])
            topics.update(m["topics"])
            events += m.get("events", [])
        self.assertEqual(topics["vehicle"], {"mode": "HOLD", "roll": 2})
        self.assertEqual([e["seq"] for e in events], [2])

        # 두 번째 화면: 위쪽 연결은 하나, 바로 최신 기준 상태
        b = self.open("s1", hz=50)
        self.assertEqual(b.block(), "retry: 2000")
        kb = b.msg()
        self.assertTrue(kb["kf"])
        self.assertEqual(kb["stream"], "up")
        self.assertEqual(kb["topics"]["vehicle"]["mode"], "HOLD")
        self.assertEqual(kb["topics"]["link"], {"loss_pct": 1.5})
        self.assertEqual([e["seq"] for e in kb["events"]], [1, 2])
        self.assertEqual(kb["events_next"], 3)
        self.assertEqual(w.accepted, 1)

        # 워커가 다시 시작: down 한 번, 다시 붙으면 새 기준 상태
        w.close()
        self.workers.remove(w)
        down = a.wait_msg(lambda m: m.get("stream") == "down", 5)
        self.assertEqual(down["topics"], {})
        self.assertFalse(down["kf"])
        self.assertIsNotNone(b.wait_msg(lambda m: m.get("stream") == "down", 5))
        w = self.worker("s1")
        up = a.wait_msg(lambda m: m.get("stream") == "up", 6)
        self.assertTrue(up["kf"])
        self.assertEqual([e["seq"] for e in up["events"]], [1])
        self.assertEqual(up["topics"], {"vehicle": {"mode": "POSCTL", "roll": 0}})

        # 화면이 모두 떠나면 위쪽 연결을 닫는다(5 s 뒤). 끊긴 화면은 쓰기에 실패해야 알므로 변경분을 몇 번 보낸다
        a.close()
        b.close()
        for i in range(6):
            w.send({"bye": {"i": i}})
            time.sleep(0.2)
        self.assertTrue(wait_for(lambda: w.alive() == 0, 12.0, 0.2))

    def test_hz_merge(self):
        w = self.worker("s2")
        a = self.open("s2", hz=2)
        self.assertIsNotNone(a.wait_msg(lambda m: m.get("stream") == "up", 5))
        for i in range(1, 31):
            w.send({"vehicle": {"mode": "M", "roll": i}}, [{"seq": i + 1, "text": "e%d" % i}])
            time.sleep(0.01)
        msgs = []
        end = time.monotonic() + 2.2
        while time.monotonic() < end:
            m = a.msg(max(0.05, end - time.monotonic()))
            if m:
                msgs.append(m)
        self.assertTrue(1 <= len(msgs) <= 4, len(msgs))
        self.assertEqual(msgs[-1]["topics"]["vehicle"]["roll"], 30)
        self.assertEqual([e["seq"] for m in msgs for e in m.get("events", [])], list(range(2, 32)))
        self.assertEqual(msgs[-1]["events_next"], 32)
        seqs = [m["seq"] for m in msgs]
        self.assertEqual(seqs, sorted(seqs))

    def test_absent_unknown_bad(self):
        a = self.open("q2", hz=10)    # Manager는 아는데 소켓이 없음 → 스트림은 열고 down
        self.assertEqual(a.status, 200)
        m = a.msg()
        self.assertEqual((m["kf"], m["stream"], m["topics"]), (True, "down", {}))
        self.assertNotIn("events", m)
        st, obj, _ = self.srv.json("GET", "/workers/nobody/stream")
        self.assertEqual((st, obj["error"]["code"]), (404, "not_found"))
        st, obj, _ = self.srv.json("GET", "/workers/s1/stream?hz=fast")
        self.assertEqual(st, 400)
        self.assertEqual(obj["error"], {"code": "bad_request", "message": "hz must be a number"})

    def test_ping(self):
        self.worker("q1")
        a = self.open("q1", hz=10)
        self.assertIsNotNone(a.wait_msg(lambda m: m.get("stream") == "up", 5))
        t0 = time.monotonic()
        blk = a.block(18.0)
        self.assertEqual(blk, ": ping")
        self.assertGreater(time.monotonic() - t0, 12.0)

    def test_slow_reader_dropped(self):
        w = self.worker("slowr")
        slow = self.open("slowr", hz=50, rcvbuf=4096)
        fast = self.open("slowr", hz=50)
        self.assertIsNotNone(fast.wait_msg(lambda m: m.get("stream") == "up", 5))
        big = "x" * 65536
        stop = []

        def flood():
            i = 0
            while not stop:
                i += 1
                w.send({"big": {"i": i, "pad": big}})
                time.sleep(0.01)
        t = threading.Thread(target=flood, daemon=True)
        t.start()
        try:
            last, gaps, count = time.monotonic(), 0.0, 0
            end = time.monotonic() + 11.0
            while time.monotonic() < end:
                m = fast.msg(3.0)
                self.assertIsNotNone(m, "fast reader starved")
                now = time.monotonic()
                gaps = max(gaps, now - last)
                last = now
                count += 1
            self.assertLess(gaps, 2.0)
            self.assertGreater(count, 50)
        finally:
            stop.append(1)
            t.join()
        # 느린 화면은 서버가 끊었다: 쌓인 것을 다 읽으면 끝(EOF)
        slow.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        end = time.monotonic() + 20
        while not slow.eof and time.monotonic() < end:
            slow._fill(1.0)
            slow.raw = slow.raw[-10:]
        self.assertTrue(slow.eof, "slow reader was not dropped")
        # 빠른 화면은 계속 받는다
        w.send({"after": {"ok": True}})
        self.assertIsNotNone(fast.wait_msg(lambda m: "after" in m["topics"], 3))


class Modes(unittest.TestCase):
    """제품 모드·토큰·화면 끔·Manager 없음·화면 파일 없음(서버를 따로 띄움)"""

    def test_restart_same_port(self):
        """게이트웨이가 요청을 받은 뒤 끝나고, 바로 같은 포트로 다시 띄운다. 앞 서버가 먼저 끊은 연결이 TIME-WAIT로
        남아도 열려야 한다(SO_REUSEADDR. 보드에서 Python REST → C++ 게이트웨이로 바꿀 때 bind 실패로 드러났던 일)"""
        first = Server(name="restart-a")
        port = first.port
        try:
            for _ in range(20):
                self.assertEqual(first.request("GET", "/health")[0], 200)
        finally:
            first.stop()
        second = Server({"WORKSPACE_REST_PORT": str(port)}, name="restart-b")
        try:
            self.assertEqual(second.request("GET", "/health")[0], 200)
        finally:
            second.stop()

    def test_product_mode(self):
        env, hdrs = {"WORKSPACE_PRODUCT": "1"}, {}
        if REST_BIN:
            # C++ 게이트웨이는 제품 모드면 신분 확인을 늘 켠다(docs/gateway.md 3장): 관리자 토큰으로 같은 것을 본다
            data = auth_data_dir()
            env.update(auth_env(data))
            hdrs = {"Authorization": "Bearer " + AUTH_USERS_TOKEN[data]}
        srv = Server(env, name="product")
        try:
            for method, path in (("GET", "/system/product-build"), ("POST", "/system/product-build"),
                                 ("GET", "/system/product-build/download"), ("DELETE", "/api/board/system/product-build/")):
                st, obj, _ = srv.json(method, path, headers=hdrs)
                self.assertEqual((st, obj), (404, {"error": {"code": "not_found", "message": "not available in product mode"}}))
            st, obj, _ = srv.json("GET", "/api/ui", headers=hdrs)
            self.assertIs(obj["product"], True)
            st, obj, _ = srv.json("GET", "/workers", headers=hdrs)
            self.assertEqual(st, 200)
        finally:
            srv.stop()

    def test_token(self):
        srv = Server({"WORKSPACE_REST_TOKEN": "s3cret"}, name="token")
        try:
            for path, hdrs, code in (("/health", {}, 200), ("/api/board/health", {}, 200), ("/workers", {}, 401),
                                     ("/workers", {"Authorization": "Bearer s3cret"}, 200),
                                     ("/workers", {"Authorization": "Bearer nope"}, 401),
                                     ("/workers", {"Cookie": "a=b; workspace_token=s3cret"}, 200),
                                     ("/workers?token=s3cret", {}, 200), ("/workers?token=x", {}, 401),
                                     ("/kit.js", {}, 200), ("/", {}, 200), ("/api/ui", {}, 401),
                                     ("/logs/workers/w1.js", {}, 401), ("/nofile.js", {}, 404)):
                with self.subTest(path=path, hdrs=hdrs):
                    st, _, data = srv.request("GET", path, headers=hdrs)
                    self.assertEqual(st, code)
                    if code == 401:
                        self.assertEqual(json.loads(data), {"error": {"code": "unauthorized", "message": "token required (Authorization: Bearer <token>)"}})
        finally:
            srv.stop()

    def test_ui_off(self):
        srv = Server({"WORKSPACE_UI": "0"}, name="uioff")
        try:
            for path in ("/", "/index.html", "/api/panels", "/api/ui", "/api/remote/commands"):
                st, obj, _ = srv.json("GET", path)
                self.assertEqual((st, obj), (404, {"error": {"code": "not_found", "message": "board UI disabled (WORKSPACE_UI=0)"}}))
            self.assertEqual(srv.json("GET", "/api/board/workers")[0], 200)
            self.assertEqual(srv.request("GET", "/kit.js")[0], 200)
            self.assertEqual(srv.json("POST", "/api/board/workers/w1/start")[0], 202)
        finally:
            srv.stop()

    def test_no_ui_files(self):
        empty = os.path.join(T["tmp"], "emptyweb")
        os.makedirs(empty, exist_ok=True)
        srv = Server({"WORKSPACE_UI_DIR": empty}, name="noui")
        try:
            st, obj, _ = srv.json("GET", "/")
            self.assertEqual((st, obj), (404, {"error": {"code": "not_found", "message": "UI files not found in %s" % empty}}))
            st, obj, _ = srv.json("GET", "/api/panels")
            self.assertEqual((st, obj), (200, [{"title": "Alpha", "id": "alpha-panel", "order": 3, "worker_types": ["a"]},
                                               {"title": "Beta", "id": "beta"},
                                               {"id": "board", "title": "Dup"}]))
        finally:
            srv.stop()

    def test_manager_down(self):
        srv = Server({"WORKSPACE_MANAGER_SOCKET": os.path.join(T["tmp"], "nomgr.sock")}, name="nomgr")
        try:
            st, obj, _ = srv.json("GET", "/health")
            self.assertEqual((st, obj), (200, {"rest": "ok", "manager": "unreachable"}))
            for method, path, body in (("GET", "/workers", None), ("POST", "/workers/w1/start", None),
                                       ("POST", "/workers/w1/command", {"cmd": "x"}), ("GET", "/workers/s1/stream", None)):
                st, obj, _ = srv.json(method, path, body)
                self.assertEqual(st, 503)
                self.assertEqual(obj["error"]["code"], "unavailable")
                self.assertEqual(obj["error"]["message"], "manager unavailable: connect: [Errno 2] No such file or directory")
            self.assertEqual(srv.json("GET", "/logs/workers/w1?lines=1")[0], 200)
            self.assertEqual(srv.json("GET", "/api/panels")[0], 200)
        finally:
            srv.stop()

    def test_manager_socket_arg(self):
        srv = Server({"WORKSPACE_MANAGER_SOCKET": os.path.join(T["tmp"], "nomgr.sock")},
                     args=["--manager-socket", T["mgr"].path], name="mgrarg")
        try:
            self.assertEqual(srv.json("GET", "/health")[1], {"rest": "ok", "manager": "ok"})
        finally:
            srv.stop()


class ProductBuild(unittest.TestCase):
    """개발 모드 제품 빌드(dev/build_product.py --local을 자식으로)"""

    def test_flow(self):
        srv = T["srv"]
        out = os.path.join(T["data"], "dist")
        st, obj, _ = srv.json("GET", "/system/product-build")
        self.assertEqual(st, 200)
        self.assertEqual(obj["state"], "idle")
        self.assertEqual(obj["log"], [])
        self.assertIsNone(obj["last"])
        self.assertEqual(obj["profiles"], ["fail", "p1", "slow"])
        st, obj, _ = srv.json("GET", "/system/product-build/download")
        self.assertEqual((st, obj["error"]), (404, {"code": "not_found", "message": "no product bundle yet"}))
        st, obj, _ = srv.json("GET", "/system/product-build/check?profile=p1")
        self.assertEqual((st, obj), (200, {"ok": True, "profile": "p1", "items": [{"key": "k", "label": "점검", "ok": True, "detail": "fine"}]}))
        for q in ("", "?profile=", "?profile=a/b"):
            self.assertEqual(srv.json("GET", "/system/product-build/check" + q)[0], 400)
        for body in (b"{bad", b'{"x": 1}', b'{"profile": "a b"}', b'{"profile": "p1", "power_mode": "bad mode"}', b'[1]'):
            with self.subTest(body=body):
                st, obj, _ = srv.json("POST", "/system/product-build", raw_body=body)
                self.assertEqual((st, obj["error"]["code"]), (400, "bad_request"))
        for method, path, code in (("PUT", "/system/product-build", 405), ("POST", "/system/product-build/check", 405),
                                   ("DELETE", "/system/product-build/download", 405), ("GET", "/system/product-build/zzz", 404),
                                   ("GET", "/system/product-builder", 404)):
            st, obj, _ = srv.json(method, path)
            self.assertEqual(st, code)

        # 취소
        st, obj, _ = srv.json("POST", "/system/product-build", {"profile": "slow"})
        self.assertEqual((st, obj["state"], obj["profile"], obj["step"]), (202, "running", "slow", "시작"))
        st, obj, _ = srv.json("POST", "/system/product-build/", {"profile": "p1"})
        self.assertEqual((st, obj["error"]["code"]), (409, "conflict"))
        self.assertTrue(wait_for(lambda: srv.json("GET", "/system/product-build")[1]["pct"] == 10, 10))
        st, obj, _ = srv.json("DELETE", "/system/product-build")
        self.assertEqual((st, obj), (200, {"cancelled": True}))
        self.assertTrue(wait_for(lambda: srv.json("GET", "/system/product-build")[1].get("finished"), 10))
        obj = srv.json("GET", "/system/product-build")[1]
        self.assertEqual((obj["state"], obj["error"]), ("cancelled", "취소됨"))
        self.assertEqual(srv.json("DELETE", "/system/product-build")[1], {"cancelled": False})

        # 실패
        srv.json("POST", "/system/product-build", {"profile": "fail"})
        self.assertTrue(wait_for(lambda: srv.json("GET", "/system/product-build")[1]["state"] != "running", 10))
        obj = srv.json("GET", "/system/product-build")[1]
        self.assertEqual((obj["state"], obj["error"]), ("failed", "boom"))

        # 성공 → 내려받기
        st, obj, _ = srv.json("POST", "/system/product-build", {"profile": "p1", "power_mode": "MAXN"})
        self.assertEqual(st, 202)
        self.assertTrue(wait_for(lambda: srv.json("GET", "/system/product-build")[1]["state"] != "running", 10))
        obj = srv.json("GET", "/system/product-build")[1]
        self.assertEqual(obj["state"], "done")
        self.assertEqual(obj["pct"], 100)
        self.assertEqual(obj["step"], "끝")
        self.assertEqual(obj["power_mode"], "MAXN")
        self.assertEqual(obj["result"], {"file": "p1-bundle.tar.gz", "power_mode": "MAXN"})
        self.assertEqual(obj["log"], ["· 준비", "plain log line", "· 끝"])
        self.assertIsNone(obj["error"])
        self.assertIsInstance(obj["elapsed"], float)
        self.assertEqual(obj["last"]["file"], "p1-bundle.tar.gz")
        self.assertEqual(obj["last"]["mb"], 0.0)
        self.assertEqual(obj["last"]["path"], os.path.join(out, "p1-bundle.tar.gz"))
        self.assertRegex(obj["last"]["created"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d$")
        self.assertEqual(list(obj)[:9], ["state", "profile", "power_mode", "pct", "step", "started", "finished", "error", "result"])
        with open(os.path.join(out, "args.json")) as f:
            self.assertEqual(json.load(f), ["p1", "--local", "--replace", "--out", out, "--power-mode", "MAXN"])
        st, hdrs, data = srv.request("GET", "/system/product-build/download")
        self.assertEqual(st, 200)
        self.assertEqual(hdrs["content-type"], "application/gzip")
        self.assertEqual(hdrs["content-disposition"], 'attachment; filename="p1-bundle.tar.gz"')
        self.assertEqual(hdrs["cache-control"], "no-store")
        self.assertEqual(data, b"BUNDLE" * 1000)


@unittest.skipUnless(REST_BIN, "C++ 게이트웨이 전용(동시 요청 상한, Unix 소켓)")
class GatewayOnly(unittest.TestCase):
    def test_concurrency_limit(self):
        w = FakeStreamWorker(os.path.join(T["data"], "run", "stream", "s3.sock"))
        srv = Server({"WORKSPACE_GATEWAY_THREADS": "2"}, name="limit")
        try:
            a = SSE(srv.port, "/workers/s3/stream")
            b = SSE(srv.port, "/workers/s3/stream")
            st, obj, _ = srv.json("GET", "/workers")
            self.assertEqual(st, 503)
            self.assertEqual(obj["error"]["code"], "unavailable")
            a.close()
            self.assertTrue(wait_for(lambda: srv.json("GET", "/workers")[0] == 200, 5))
            b.close()
        finally:
            srv.stop()
            w.close()

    def test_unix_socket(self):
        path = os.path.join(T["tmp"], "gw.sock")
        srv = Server({"WORKSPACE_GATEWAY_SOCKET": path}, name="unix")
        try:
            self.assertTrue(wait_for(lambda: os.path.exists(path), 5))
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(path)
            s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
            buf = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
            s.close()
            head, body = buf.split(b"\r\n\r\n", 1)
            self.assertTrue(head.startswith(b"HTTP/1.1 200"))
            self.assertEqual(json.loads(body), {"rest": "ok", "manager": "ok"})
            self.assertEqual(srv.json("GET", "/health")[0], 200)  # TCP도 그대로
        finally:
            srv.stop()

    def test_unix_only(self):
        """WORKSPACE_GATEWAY_TCP=0(nginx 뒤): TCP 포트는 열지 않고 Unix 소켓으로만 답한다. 소켓 없이 0이면 시작 실패"""
        path = os.path.join(T["tmp"], "gw-only.sock")
        srv = Server({"WORKSPACE_GATEWAY_SOCKET": path, "WORKSPACE_GATEWAY_TCP": "0"}, name="unixonly", unix_only=path)
        try:
            with self.assertRaises(OSError):
                socket.create_connection(("127.0.0.1", srv.port), 0.5).close()
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(path)
            s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
            buf = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
            s.close()
            self.assertTrue(buf.startswith(b"HTTP/1.1 200"))
        finally:
            srv.stop()
        with self.assertRaises(RuntimeError):
            Server({"WORKSPACE_GATEWAY_TCP": "0"}, name="unixonly-bad", unix_only=os.path.join(T["tmp"], "never.sock"))


# ---- 4단계: 신분 확인·역할·운용/정비 모드·명령 기록·Manager 캐시 (C++ 게이트웨이만, docs/gateway.md 3장) ----

UNAUTH = {"code": "unauthorized", "message": "login required (POST /api/auth/login or Authorization: Bearer <token>)"}


@unittest.skipUnless(REST_BIN, "C++ 게이트웨이 전용(신분 확인)")
class AuthOn(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = auth_data_dir()
        cls.srv = Server(auth_env(cls.data), name="auth")

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()

    def setUp(self):
        T["mgr"].mode = None

    def test_auth_off_unchanged(self):
        """WORKSPACE_AUTH=off(기본): 지금처럼 누구나, me는 auth off·maintainer, 로그인은 400"""
        srv = T["srv"]
        st, obj, _ = srv.json("GET", "/api/auth/me")
        self.assertEqual((st, obj), (200, {"auth": "off", "name": None, "role": "maintainer", "mode": "operation"}))
        self.assertEqual(srv.json("GET", "/workers")[0], 200)
        self.assertEqual(srv.json("POST", "/workers/w1/start")[0], 202)
        st, obj, _ = srv.json("POST", "/api/auth/login", {"name": "m1", "password": "x"})
        self.assertEqual((st, obj["error"]["code"]), (400, "bad_request"))
        self.assertEqual(srv.json("POST", "/api/board/auth/logout")[0], 200)

    def test_anonymous(self):
        srv = self.srv
        for method, path, code in (("GET", "/workers", 401), ("GET", "/api/board/system", 401), ("GET", "/api/ui", 401),
                                   ("GET", "/api/panels", 401), ("GET", "/workers/s1/stream", 401),
                                   ("POST", "/workers/w1/command", 401), ("POST", "/workers/w1/start", 401),
                                   ("PUT", "/api/mode", 401), ("GET", "/health", 200), ("GET", "/api/board/health", 200),
                                   ("GET", "/", 200), ("GET", "/kit.js", 200), ("GET", "/shell.css", 200)):
            with self.subTest(method=method, path=path):
                st, _, data = srv.request(method, path, {"cmd": "x"} if method == "POST" else None)
                self.assertEqual(st, code)
                if code == 401:
                    self.assertEqual(json.loads(data), {"error": UNAUTH})
        st, obj, _ = srv.json("GET", "/api/auth/me")
        self.assertEqual((st, obj), (401, {"error": {"code": "unauthorized", "message": "login required"}, "auth": "on"}))

    def test_login_logout(self):
        s = Session(self.srv)
        for name, pw in (("m1", "wrong-pass"), ("nobody", "maint-pass-1")):
            st, obj, _ = s.login(name, pw)
            self.assertEqual((st, obj["error"]["code"]), (401, "unauthorized"))
        for body in ({"name": "m1"}, {"password": "x"}, [1], {"name": 1, "password": "x"}):
            st, obj, _ = self.srv.json("POST", "/api/auth/login", body)
            self.assertEqual(st, 400, body)
        st, obj, hdrs = s.login("m1", PASSWORDS["m1"])
        self.assertEqual((st, obj), (200, {"auth": "on", "name": "m1", "role": "maintainer", "mode": "operation"}))
        cookie = hdrs["set-cookie"]
        for part in ("HttpOnly", "SameSite=Strict", "Path=/", "Max-Age=604800"):
            self.assertIn(part, cookie)
        self.assertEqual(len(s.cookie), 64)
        st, obj, _ = s.json("GET", "/api/auth/me")
        self.assertEqual((st, obj), (200, {"auth": "on", "name": "m1", "role": "maintainer", "mode": "operation", "via": "session"}))
        self.assertEqual(s.json("GET", "/workers")[0], 200)
        self.assertEqual(s.json("GET", "/api/board/workers/w1")[0], 200)
        st, hdrs, _ = self.srv.request("POST", "/api/auth/logout", headers=s.headers())
        self.assertEqual(st, 200)
        self.assertIn("Max-Age=0", hdrs["set-cookie"])
        self.assertEqual(s.json("GET", "/workers")[0], 401)  # 지운 세션은 다시 못 씀
        self.assertEqual(Session(self.srv).json("GET", "/workers", headers={"Cookie": "workspace_session=" + "0" * 64})[0], 401)

    def test_role_matrix(self):
        sample = [  # (메서드, 경로, 본문, 필요한 권한)
            ("GET", "/workers", None, "read"), ("GET", "/api/board/system", None, "read"), ("GET", "/api/mode", None, "read"),
            ("GET", "/logs/workers/w1?lines=1", None, "read"), ("GET", "/api/panels", None, "read"),
            ("GET", "/api/ui", None, "read"), ("GET", "/workers/w1/io", None, "read"),
            ("POST", "/workers/w2/command", {"cmd": "x"}, "operate"), ("POST", "/api/board/workers/w2/command", {"cmd": "y"}, "operate"),
            ("POST", "/workers/w1/start", None, "maintain"), ("POST", "/workers/w1/stop", None, "maintain"),
            ("PUT", "/workers/w1/config", {"a": 1}, "maintain"), ("PUT", "/workers/w1/autostart", {"autostart": True}, "maintain"),
            ("POST", "/workers/w1/presets", {"name": "x"}, "maintain"), ("PUT", "/api/mode", {"mode": "operation"}, "maintain"),
            ("GET", "/system/product-build", None, "maintain"),
        ]
        allowed = {"viewer": {"read"}, "operator": {"read", "operate"}, "maintainer": {"read", "operate", "maintain"}}
        for name, role in ROLES.items():
            s = Session(self.srv)
            self.assertEqual(s.login(name, PASSWORDS[name])[0], 200)
            for method, path, body, need in sample:
                with self.subTest(user=name, method=method, path=path):
                    st, obj, _ = s.json(method, path, body)
                    if need in allowed[role]:
                        self.assertLess(st, 300, obj)
                    else:
                        self.assertEqual(st, 403, obj)
                        self.assertEqual(obj["error"]["code"], "forbidden")
                        self.assertIn("role " + role, obj["error"]["message"])

    def test_bearer_token(self):
        tok = AUTH_USERS_TOKEN[self.data]
        s = Session(self.srv, token=tok)
        st, obj, _ = s.json("GET", "/api/auth/me")
        self.assertEqual((st, obj["name"], obj["role"], obj["via"]), (200, "m1", "maintainer", "token"))
        self.assertEqual(s.json("POST", "/workers/w1/start")[0], 202)
        self.assertEqual(self.srv.json("GET", "/workers?token=" + tok)[0], 200)  # EventSource용(GET만)
        self.assertEqual(self.srv.json("POST", "/workers/w1/start?token=" + tok)[0], 401)
        self.assertEqual(Session(self.srv, token=tok[:-1] + "x").json("GET", "/workers")[0], 401)
        # 틀린 토큰이면 쿠키가 맞아도 익명(섞어 쓰지 않음)
        c = Session(self.srv)
        c.login("v1", PASSWORDS["v1"])
        c.token = "nope"
        self.assertEqual(c.json("GET", "/workers")[0], 401)
        # 다른 토큰을 만들고 지우면 다시 시작하지 않아도 바로(1 s 안팎) 거절
        r = gw_cli(self.data, "token", "add", "o1", "tmp")
        self.assertEqual(r.returncode, 0, r.stderr)
        t2 = r.stdout.strip()
        self.assertEqual(len(t2), 64)
        self.assertNotIn(t2, slurp(os.path.join(self.data, "config", "users.json")))  # 파일에는 해시만
        s2 = Session(self.srv, token=t2)
        self.assertTrue(wait_for(lambda: s2.json("GET", "/workers")[0] == 200, 5))
        self.assertEqual(s2.json("POST", "/workers/w1/start")[0], 403)  # o1은 operator
        self.assertEqual(gw_cli(self.data, "token", "del", "o1", "tmp").returncode, 0)
        self.assertTrue(wait_for(lambda: s2.json("GET", "/workers")[0] == 401, 5))
        self.assertEqual(s.json("GET", "/workers")[0], 200)  # 다른 토큰은 그대로

    def test_users_file_reload(self):
        """사용자 파일을 바꾸면 다시 시작하지 않아도 반영: 새 사용자, 비밀번호 바꿈·지움은 그 세션을 끝냄"""
        # 다시 읽었는지는 /health?detail=1의 사용자 수로 본다(로그인을 되풀이하면 실패 제한에 걸림)
        n = users_count(self.srv)
        add_user(self.data, "late", "viewer", "late-pass-1")
        self.assertTrue(wait_for(lambda: users_count(self.srv) == n + 1, 5))
        s = Session(self.srv)
        self.assertEqual(s.login("late", "late-pass-1")[0], 200)
        self.assertEqual(s.json("GET", "/workers")[0], 200)
        r = gw_cli(self.data, "user", "passwd", "late", stdin="late-pass-2\nlate-pass-2\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(wait_for(lambda: s.json("GET", "/workers")[0] == 401, 5))
        self.assertEqual(Session(self.srv).login("late", "late-pass-1")[0], 401)
        s2 = Session(self.srv)
        self.assertEqual(s2.login("late", "late-pass-2")[0], 200)
        self.assertEqual(gw_cli(self.data, "user", "del", "late").returncode, 0)
        self.assertTrue(wait_for(lambda: s2.json("GET", "/workers")[0] == 401, 5))
        self.assertEqual(users_count(self.srv), n)
        # 설정 명령의 오류: 짧은 비밀번호, 다른 두 번, 있는 이름, 없는 역할
        for args, stdin in ((("user", "add", "x1", "viewer"), "short\nshort\n"), (("user", "add", "x1", "viewer"), "abcdefgh1\nabcdefgh2\n"),
                            (("user", "add", "m1", "viewer"), "abcdefgh1\nabcdefgh1\n"), (("user", "add", "x1", "admin"), "abcdefgh1\nabcdefgh1\n"),
                            (("user", "del", "nobody"), None), (("token", "add", "nobody", "t"), None)):
            r = gw_cli(self.data, *args, stdin=stdin)
            self.assertEqual(r.returncode, 1, (args, r.stdout, r.stderr))
        self.assertEqual(gw_cli(self.data, "user").returncode, 2)
        r = gw_cli(self.data, "user", "list")
        self.assertIn("m1\tmaintainer\tpassword\ttokens: ci", r.stdout)
        users = json.loads(slurp(os.path.join(self.data, "config", "users.json")))
        self.assertEqual(os.stat(os.path.join(self.data, "config", "users.json")).st_mode & 0o777, 0o600)
        m1 = [u for u in users["users"] if u["name"] == "m1"][0]
        self.assertTrue(m1["password_hash"].startswith("pbkdf2-sha256$1000$"))
        self.assertNotIn(PASSWORDS["m1"], json.dumps(users))

    def test_default_iterations(self):
        """반복 수를 정하지 않으면 200000(docs/gateway.md 3장)"""
        data = tempfile.mkdtemp(prefix="iter", dir=T["tmp"])
        env = dict(os.environ, WORKSPACE_DATA=data)
        env.pop("WORKSPACE_AUTH_ITERATIONS", None)
        r = subprocess.run([REST_BIN, "user", "add", "a1", "viewer"], input="pass-word-1\npass-word-1\n",
                           capture_output=True, text=True, env=env, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        h = json.loads(slurp(os.path.join(data, "config", "users.json")))["users"][0]["password_hash"]
        self.assertTrue(h.startswith("pbkdf2-sha256$200000$"), h)


@unittest.skipUnless(REST_BIN, "C++ 게이트웨이 전용(신분 확인)")
class AuthLimits(unittest.TestCase):
    def test_rate_limit(self):
        """주소마다 5번 틀리면 30 s 막힘. X-Forwarded-For는 nginx 소켓(Unix)으로 온 것만 믿는다"""
        data = auth_data_dir()
        sock = os.path.join(data, "gw.sock")
        srv = Server(auth_env(data, WORKSPACE_GATEWAY_SOCKET=sock), name="ratelimit")
        try:
            for i in range(5):
                st, obj, _ = Session(srv).login("o1", "wrong-%d" % i)
                self.assertEqual(st, 401)
            st, obj, hdrs = Session(srv).login("o1", PASSWORDS["o1"])
            self.assertEqual((st, obj["error"]["code"]), (429, "too_many_requests"))
            self.assertTrue(1 <= int(hdrs["retry-after"]) <= 30)
            # TCP로 온 X-Forwarded-For는 무시(직접 붙은 쪽이 꾸밀 수 있음)
            st, _, _ = srv.request("POST", "/api/auth/login", {"name": "o1", "password": PASSWORDS["o1"]},
                                   headers={"X-Forwarded-For": "10.1.2.3"})
            self.assertEqual(st, 429)
            # nginx(Unix 소켓) 뒤의 다른 주소는 막히지 않음, 같은 주소는 막힘
            self.assertTrue(wait_for(lambda: os.path.exists(sock), 5))
            body = {"name": "o1", "password": PASSWORDS["o1"]}
            self.assertEqual(unix_json(sock, "POST", "/api/auth/login", body, {"X-Forwarded-For": "10.1.2.3"})[0], 200)
            for i in range(5):
                unix_json(sock, "POST", "/api/auth/login", {"name": "o1", "password": "bad"}, {"X-Forwarded-For": "10.9.9.9"})
            self.assertEqual(unix_json(sock, "POST", "/api/auth/login", body, {"X-Forwarded-For": "10.9.9.9"})[0], 429)
            self.assertEqual(unix_json(sock, "POST", "/api/auth/login", body, {"X-Forwarded-For": "10.1.2.4"})[0], 200)
        finally:
            srv.stop()
        lines = log_lines(data)
        self.assertIn("10.9.9.9", {l["remote"] for l in lines})
        self.assertIn("127.0.0.1", {l["remote"] for l in lines})

    def test_product_forces_auth(self):
        """제품 모드: WORKSPACE_AUTH=off여도 켬. 사용자 파일이 없으면 /health 말고 모두 503, 사용자를 넣으면 다시 시작 없이 열림"""
        data = tempfile.mkdtemp(prefix="prod", dir=T["tmp"])
        srv = Server(auth_env(data, WORKSPACE_PRODUCT="1", WORKSPACE_AUTH="off", WORKSPACE_LOG_LEVEL="INFO"), name="prodauth")
        try:
            self.assertEqual(srv.json("GET", "/health")[0], 200)
            self.assertEqual(srv.json("GET", "/api/board/health")[0], 200)
            for method, path in (("GET", "/workers"), ("GET", "/"), ("GET", "/kit.js"), ("GET", "/api/auth/me"),
                                 ("POST", "/api/auth/login"), ("PUT", "/api/mode")):
                with self.subTest(path=path):
                    st, obj, _ = srv.json(method, path)
                    self.assertEqual((st, obj["error"]["code"]), (503, "unavailable"))
            log = slurp(srv.log_path)
            self.assertIn("authentication forced on", log)
            self.assertIn("no users in", log)
            add_user(data, "m1", "maintainer", PASSWORDS["m1"])
            self.assertTrue(wait_for(lambda: users_count(srv) == 1, 5))
            s = Session(srv)
            self.assertEqual(s.login("m1", PASSWORDS["m1"])[0], 200)
            self.assertEqual(s.json("GET", "/workers")[0], 200)
            self.assertEqual(srv.json("GET", "/workers")[0], 401)
        finally:
            srv.stop()


@unittest.skipUnless(REST_BIN, "C++ 게이트웨이 전용(운용/정비 모드)")
class AccessMode(unittest.TestCase):
    def test_mode(self):
        data = auth_data_dir()
        srv = Server(auth_env(data), name="mode")
        try:
            m, o = Session(srv), Session(srv)
            m.login("m1", PASSWORDS["m1"])
            o.login("o1", PASSWORDS["o1"])
            st, obj, _ = o.json("GET", "/api/mode")
            self.assertEqual((st, obj), (200, {"mode": "operation", "since": None, "by": None}))
            st, obj, _ = o.json("PUT", "/api/mode", {"mode": "maintenance"})
            self.assertEqual((st, obj["error"]["code"]), (403, "forbidden"))
            for bad in ({"mode": "maint"}, {"mode": None}, {}, ["maintenance"]):
                self.assertEqual(m.json("PUT", "/api/mode", bad)[0], 400, bad)
            n = len(T["mgr"].requests)
            st, obj, _ = m.json("PUT", "/api/board/mode", {"mode": "maintenance"})
            self.assertEqual(st, 200)
            self.assertEqual((obj["mode"], obj["by"], obj["manager"]), ("maintenance", "m1", "ok"))
            self.assertRegex(obj["since"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}[+-]\d\d:\d\d$")
            self.assertIn({"op": "set_access_mode", "mode": "maintenance"}, T["mgr"].requests[n:])  # process-manager에 알림
            st, obj, _ = o.json("GET", "/api/mode")
            self.assertEqual((obj["mode"], obj["by"]), ("maintenance", "m1"))
            self.assertEqual(o.json("GET", "/api/auth/me")[1]["mode"], "maintenance")
            self.assertEqual(m.json("DELETE", "/api/mode")[0], 405)
        finally:
            srv.stop()
        # 저장하지 않음: 다시 시작하면 운용
        srv = Server(auth_env(data), name="mode2")
        try:
            s = Session(srv)
            s.login("v1", PASSWORDS["v1"])
            self.assertEqual(s.json("GET", "/api/mode")[1], {"mode": "operation", "since": None, "by": None})
        finally:
            srv.stop()
        cmds = [l["cmd"] for l in log_lines(data)]
        self.assertIn("mode:maintenance", cmds)

    def test_manager_down(self):
        """process-manager가 없어도 모드는 바뀜(manager: unreachable), 다시 붙으면 gw.mgr가 맞춤"""
        data = tempfile.mkdtemp(prefix="modemgr", dir=T["tmp"])
        path = os.path.join(data, "sub-manager.sock")
        srv = Server({"WORKSPACE_DATA": data, "WORKSPACE_MANAGER_SOCKET": path}, name="modemgr")
        try:
            st, obj, _ = srv.json("PUT", "/api/mode", {"mode": "maintenance"})  # 인증 꺼짐: 누구나
            self.assertEqual((st, obj["mode"]), (200, "maintenance"))
            self.assertTrue(obj["manager"].startswith("unreachable"))
            fake = SubManager(path)  # 이제 뜬 Manager는 운용 모드(다시 시작한 process-manager처럼)
            try:
                self.assertTrue(wait_for(lambda: fake.count("set_access_mode") >= 1, 8))
                with fake.lock:
                    self.assertEqual([r for r in fake.ops if r.get("op") == "set_access_mode"][0]["mode"], "maintenance")
            finally:
                fake.close()
        finally:
            srv.stop()


@unittest.skipUnless(REST_BIN, "C++ 게이트웨이 전용(명령 기록)")
class CommandLog(unittest.TestCase):
    def test_log_lines(self):
        data = auth_data_dir()
        tok = AUTH_USERS_TOKEN[data]
        srv = Server(auth_env(data), name="cmdlog")
        try:
            Session(srv).login("m1", "secret-wrong-pass")       # 틀린 비밀번호(있는 이름)
            Session(srv).login("ghost-name", "another-secret")   # 없는 이름: 이름도 남기지 않음
            o = Session(srv)
            o.login("o1", PASSWORDS["o1"])
            self.assertEqual(o.json("POST", "/workers/w2/command", {"cmd": "source.set_rate", "args": {"password": "argsecret"}})[0], 200)
            self.assertEqual(o.json("POST", "/workers/w1/start")[0], 403)
            m = Session(srv, token=tok)
            self.assertEqual(m.json("PUT", "/workers/w1/config?token=" + tok, {"key": "v"})[0], 202)
            self.assertEqual(m.json("PUT", "/api/mode", {"mode": "maintenance"})[0], 200)
            self.assertEqual(m.json("POST", "/workers/w1/stop")[0], 202)
            self.assertEqual(srv.json("POST", "/workers/w1/start")[0], 401)
            for _ in range(3):
                o.json("GET", "/workers")  # 읽기는 남기지 않음
            self.assertEqual(srv.request("POST", "/api/auth/logout", headers=o.headers())[0], 200)
            st, obj, _ = srv.json("GET", "/health?detail=1")
            g = obj["gateway"]
            self.assertEqual((g["auth"], g["mode"]["mode"], g["log"]["dropped"]), ("on", "maintenance", 0))
            self.assertTrue(wait_for(lambda: srv.json("GET", "/health?detail=1")[1]["gateway"]["log"]["written"] >= 10, 5))
        finally:
            srv.stop()
        lines = log_lines(data)
        keys = {"t", "user", "role", "mode", "remote", "method", "path", "cmd", "code", "ms"}
        for l in lines:
            self.assertEqual(set(l), keys, l)
        got = [(l["user"], l["role"], l["mode"], l["method"], l["path"], l["cmd"], l["code"]) for l in lines]
        self.assertEqual(got, [
            ("m1", "-", "operation", "POST", "/api/auth/login", "login", 401),
            ("?", "-", "operation", "POST", "/api/auth/login", "login", 401),
            ("o1", "operator", "operation", "POST", "/api/auth/login", "login", 200),
            ("o1", "operator", "operation", "POST", "/workers/w2/command", "source.set_rate", 200),
            ("o1", "operator", "operation", "POST", "/workers/w1/start", None, 403),
            ("m1", "maintainer", "operation", "PUT", "/workers/w1/config", "set_config", 202),
            ("m1", "maintainer", "operation", "PUT", "/api/mode", "mode:maintenance", 200),
            ("m1", "maintainer", "maintenance", "POST", "/workers/w1/stop", "stop", 202),
            ("-", "-", "maintenance", "POST", "/workers/w1/start", None, 401),
            ("o1", "operator", "maintenance", "POST", "/api/auth/logout", "logout", 200),
        ])
        self.assertEqual({l["remote"] for l in lines}, {"127.0.0.1"})
        text = log_text(data)
        for secret in list(PASSWORDS.values()) + [tok, "secret-wrong-pass", "another-secret", "ghost-name", "argsecret", "token="]:
            self.assertNotIn(secret, text)
        name = os.listdir(os.path.join(data, "records", "gateway"))
        self.assertEqual(len(name), 1)
        self.assertRegex(name[0], r"^gateway-[0-9a-f]{8}\.jsonl$")
        self.assertFalse([f for f in os.listdir(os.path.join(data, "records")) if f.endswith(".jsonl")])  # logger 폴더와 나눔
        self.assertEqual(os.stat(os.path.join(data, "records", "gateway", name[0])).st_mode & 0o777, 0o600)


@unittest.skipUnless(REST_BIN, "C++ 게이트웨이 전용(Manager 캐시)")
class ManagerCache(unittest.TestCase):
    def test_cache(self):
        data = tempfile.mkdtemp(prefix="cache", dir=T["tmp"])
        path = os.path.join(data, "sub-manager.sock")
        fake = SubManager(path)
        srv = Server({"WORKSPACE_DATA": data, "WORKSPACE_MANAGER_SOCKET": path}, name="cache")
        try:
            detail = lambda: srv.json("GET", "/health?detail=1")[1]["gateway"]["cache"]
            self.assertTrue(wait_for(lambda: detail()["fresh"], 8), detail())
            # 직접 부른 답과 같은 모양(GET /workers는 list 순서 그대로)
            lists = fake.count("list")
            for _ in range(10):
                st, _, body = srv.request("GET", "/workers")
                self.assertEqual(st, 200)
                self.assertEqual(json.loads(body), fake.workers)
            self.assertEqual(body.decode(), json.dumps(fake.workers, ensure_ascii=False))
            self.assertEqual(srv.json("GET", "/api/board/workers/w1")[1], fake.workers[1])
            self.assertEqual(srv.json("GET", "/system")[1], fake.system)
            self.assertEqual(fake.count("list"), lists)  # 화면 요청이 Manager list로 가지 않음
            self.assertEqual(fake.count("system"), 0)
            # 없는 워커는 Manager에 직접(not_found)
            self.assertEqual(srv.json("GET", "/workers/nope")[0], 404)
            # Manager가 잠깐 멈춤: 3 s 안에는 캐시로 바로 답한다
            fake.stall = True
            t0 = time.monotonic()
            self.assertEqual(srv.json("GET", "/workers")[0], 200)
            self.assertLess(time.monotonic() - t0, 0.5)
            # 오래되면 직접 묻는다(지금처럼): 멈춘 Manager라 응답 상한(2 s) 뒤 503
            time.sleep(3.3)
            self.assertFalse(detail()["fresh"])
            st, obj, _ = srv.json("GET", "/workers")
            self.assertEqual((st, obj["error"]["code"]), (503, "unavailable"))
            fake.stall = False
            self.assertTrue(wait_for(lambda: detail()["fresh"], 10))
            self.assertEqual(srv.json("GET", "/workers")[1], fake.workers)
            # 쓰기는 늘 직접
            n = fake.count("start")
            self.assertEqual(srv.json("POST", "/workers/w1/start")[0], 202)
            self.assertEqual(fake.count("start"), n + 1)
        finally:
            srv.stop()
            fake.close()

    def test_cache_off(self):
        data = tempfile.mkdtemp(prefix="nocache", dir=T["tmp"])
        path = os.path.join(data, "sub-manager.sock")
        fake = SubManager(path)
        srv = Server({"WORKSPACE_DATA": data, "WORKSPACE_MANAGER_SOCKET": path, "WORKSPACE_GATEWAY_CACHE": "0"}, name="nocache")
        try:
            for _ in range(3):
                self.assertEqual(srv.json("GET", "/workers")[1], fake.workers)
            self.assertEqual(fake.count("list"), 3)
            self.assertEqual(fake.count("subscribe"), 0)
            self.assertIsNone(srv.json("GET", "/health?detail=1")[1]["gateway"]["cache"])
        finally:
            srv.stop()
            fake.close()


if __name__ == "__main__":
    unittest.main()
