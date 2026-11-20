"""출력 대조(워크스페이스 design.md 6.6.7.2): 워커가 실제로 정의대로 내보내는지 본다. Linux, 표준 라이브러리만.

워커마다(io.json에 check가 있는 것) 워크스페이스 연동(ON) 빌드를 check.preset(가짜 장치)으로 단독 실행하고
  - 스트림을 listen_s 동안 구독: 줄마다 공통 정의의 스트림 겉모양, 주제 이름이 정의에 있는지, 주제 값(t_mono_ns 포함)
  - get_status: 칸마다 공통 정의 status + 워커 정의 status, 빠진 칸(optional 아님)
  - check.calls의 명령: 응답 data가 정의의 reply, 오류 code가 정의의 errors
  - heartbeat 지표: 이름이 정의에 있는지, 값 형식, 한 번도 안 온 지표(optional 아님)
정의 밖 칸·주제·지표는 실패. 정의에 있는데 한 번도 안 나온 주제는 알림.

    WORKSPACE_CHECK_OUT=/tmp/workspace-check/<저장소> python3 -m unittest dev/test_io_output.py -v   (dev/check.sh가 부른다)

빌드가 없으면(WORKSPACE_CHECK_OUT 없음, 실행 파일 없음) 그 워커는 건너뛴다.
"""
import glob
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import io_contract as ic  # noqa: E402

sys.path.insert(0, os.path.join(ic.TEMPLATE, "python"))
import workspace_worker as ww  # noqa: E402

OUT = os.environ.get("WORKSPACE_CHECK_OUT", "")


def checked_workers():
    out = []
    for w in sorted(glob.glob(os.path.join(ic.ROOT, "workers", "*"))):
        p = os.path.join(w, "io.json")
        if os.path.isdir(w) and os.path.exists(p) and "check" in ic.load_json(p):
            out.append(w)
    return out


def find_exec(name):
    if not OUT:
        return None
    for p in sorted(glob.glob(os.path.join(OUT, name + "-ON", "*-worker"))):
        if os.access(p, os.X_OK) and os.path.isfile(p):
            return p
    return None


def wait_for(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.05)
    return fn()


class StreamReader:
    """스트림 소켓에서 줄을 그대로 읽는다(겉모양까지 검사하려고 도우미의 Subscriber 대신)"""

    def __init__(self, path):
        self.path, self.sock, self.buf, self.lines = path, None, b"", []

    def pump(self):
        if self.sock is None:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.path)
                s.setblocking(False)
                self.sock = s
            except OSError:
                return
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    self.sock.close()
                    self.sock = None
                    return
                self.buf += chunk
        except BlockingIOError:
            pass
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            if line.strip():
                self.lines.append(line.decode("utf-8", "replace"))

    def close(self):
        if self.sock:
            self.sock.close()


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux only")
class OutputMatchesDefinition(unittest.TestCase):
    def test_workers(self):
        ws = checked_workers()
        self.assertTrue(ws, "check가 있는 워커가 없음(틀 workers/_template에는 있어야 함)")
        for w in ws:
            name = os.path.basename(w)
            exe = find_exec(name)
            with self.subTest(worker=name):
                if not exe:
                    print("\n%s: 빌드 없음, 건너뜀" % name)
                    continue
                errs, notes = run_check(w, exe)
                errs = list(dict.fromkeys(errs))  # 같은 틀림이 줄마다 반복되면 한 번만
                for n in notes:
                    print("\n%s: 알림: %s" % (name, n))
                self.assertEqual(errs, [], "%s 출력이 정의와 다름:\n  %s" % (name, "\n  ".join(errs[:40])))


def run_check(w, exe):
    contract = ic.Contract(w)
    check = contract.io["check"]
    preset = ic.load_json(os.path.join(w, "workspace", "presets", check["preset"] + ".json"))
    errs, notes = [], []
    tmp = tempfile.mkdtemp(prefix="io-check-")
    env = dict(os.environ, WORKSPACE_DATA=tmp, XDG_RUNTIME_DIR=tmp)
    for d in ("run/cmd", "run/stream", "run/frames"):
        os.makedirs(os.path.join(tmp, d))
    notify_path = os.path.join(tmp, "notify.sock")
    notify = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    notify.bind(notify_path)
    notify.setblocking(False)
    env["WORKSPACE_NOTIFY_SOCKET"] = notify_path
    wid = "chk"
    cfg_path = os.path.join(tmp, wid + ".json")
    with open(cfg_path, "w") as f:
        json.dump({"id": wid, "config_version": 1, "config": preset["config"]}, f)
    log = open(os.path.join(tmp, "worker.log"), "w")
    proc = subprocess.Popen([exe, "--config", cfg_path], env=env, stdout=log, stderr=subprocess.STDOUT)
    old = {k: os.environ.get(k) for k in ("WORKSPACE_DATA", "XDG_RUNTIME_DIR")}
    os.environ.update(WORKSPACE_DATA=tmp, XDG_RUNTIME_DIR=tmp)  # 도우미가 소켓 위치를 같은 규칙으로 찾게
    reader = StreamReader(ww.default_stream_socket(wid))
    heartbeats = []
    try:
        cmd_sock = ww.default_command_socket(wid)
        if not wait_for(lambda: ww.send_command(cmd_sock, "get_status").get("ok"), 10):
            errs.append("워커가 뜨지 않음(로그 %s)" % os.path.join(tmp, "worker.log"))
            return errs, notes
        end = time.time() + float(check.get("listen_s", 3))
        calls = list(check.get("calls", []))
        replies = []
        while time.time() < end:
            reader.pump()
            try:
                while True:
                    heartbeats.append(notify.recv(65536).decode("utf-8", "replace"))
            except BlockingIOError:
                pass
            if calls:
                c = calls.pop(0)
                replies.append((c["cmd"], ww.send_command(cmd_sock, c["cmd"], c.get("args", {}))))
            time.sleep(0.05)
        replies.append(("get_status", ww.send_command(cmd_sock, "get_status")))
        reader.pump()
        check_stream(contract, reader.lines, errs, notes)
        for cmd, r in replies:
            check_reply(contract, cmd, r, errs)
        check_metrics(contract, [h for h in heartbeats if h.startswith("WATCHDOG=1")], errs)
    finally:
        reader.close()
        for k, v in old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(10)
            if rc != 0:
                errs.append("SIGTERM 뒤 종료 코드 %s(로그 %s)" % (rc, os.path.join(tmp, "worker.log")))
        except subprocess.TimeoutExpired:
            proc.kill()
            errs.append("SIGTERM 뒤 10 s 안에 안 끝남")
        log.close()
        notify.close()
        if not errs:
            shutil.rmtree(tmp, ignore_errors=True)
    return errs, notes


def check_stream(c, lines, errs, notes):
    seen = set()
    if not lines and c.topics.items():  # 주제를 정의한 워커만 스트림이 있어야 함
        errs.append("스트림: 받은 줄 없음")
    for i, line in enumerate(lines):
        try:
            msg = json.loads(line)
        except ValueError:
            errs.append("스트림 %d번째 줄: JSON 아님" % i)
            continue
        ic.validate(msg, c.common["stream"], c.defs, "스트림", errs)
        for name, value in (msg.get("topics") or {}).items():
            key, spec = c.topics.find(name)
            if spec is None:
                errs.append("스트림 주제 %s: 정의에 없음" % name)
                continue
            seen.add(key)
            if value is None:
                continue
            if isinstance(value, dict) and "t_mono_ns" not in value:
                errs.append("주제 %s: t_mono_ns 없음(공통 정의: 주제 값마다 그 값의 시각)" % name)
            ic.validate(value, spec.get("schema"), c.defs, "주제 " + name, errs)
    for name, _ in c.topics.items():
        if name not in seen:
            notes.append("주제 %s가 한 번도 안 나옴(시험이 그 길을 안 지났거나 정의가 남음)" % name)


def check_reply(c, cmd, r, errs):
    key, spec = c.commands.find(cmd)
    if spec is None:
        errs.append("명령 %s: 정의에 없음" % cmd)
        return
    if r.get("ok"):
        data = r.get("data", {})
        if cmd == "get_status":
            check_status(c, data, errs)
        else:
            ic.validate(data, spec.get("reply"), c.defs, "명령 %s 응답" % cmd, errs)
    else:
        code = (r.get("error") or {}).get("code")
        if code not in spec.get("errors", []):
            errs.append("명령 %s: 오류 %s가 정의의 errors %s에 없음(%s)" % (cmd, code, spec.get("errors", []), r))


def check_status(c, data, errs):
    for k, v in data.items():
        key, spec = c.status.find(k)
        if spec is None:
            errs.append("get_status.%s: 정의에 없음(공통 status 또는 워커 정의 status)" % k)
            continue
        ic.validate(v, spec, c.defs, "get_status." + k, errs)
    for k, spec in c.status.items():
        if "<" not in k and not spec.get("optional") and k not in data:
            errs.append("get_status.%s: 없음(optional 아님)" % k)


def check_metrics(c, beats, errs):
    if not beats:
        errs.append("heartbeat: 받은 것 없음")
        return
    seen = set()
    for b in beats:
        for k, v in ic.parse_heartbeat(b).items():
            key, spec = c.metrics.find(k)
            if spec is None:
                errs.append("지표 %s: 정의에 없음" % k)
                continue
            seen.add(key)
            ic.validate(v, spec, c.defs, "지표 " + k, errs)
    for k, spec in c.metrics.items():
        if "<" not in k and not spec.get("optional") and k not in seen:
            errs.append("지표 %s: 한 번도 안 옴(optional 아님)" % k)


if __name__ == "__main__":
    unittest.main()
