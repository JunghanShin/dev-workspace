"""record-manager ↔ 예전 workspace-logger 줄 비교(managers/record/design.md 4장).

가짜 process-manager 소켓에 record-manager를 붙이고 정해진 구독 메시지를 보낸 뒤, 기록 줄을 정답 파일
test/logger_expected.jsonl과 맞댄다. 정답 파일은 Python workspace-logger를 지우기 전(2026-11-24, 4단계)에
같은 메시지로 그 logger가 쓴 줄이다(t만 뺌).
  - logger 종류(start·connected·stop)를 뺀 줄은 글자 그대로 같아야 한다(t·t_mono_ns만 뺌):
    게이트웨이 /history가 줄을 글자로 찾기 때문(예: "type":"worker")
  - 같은 개수·같은 순서

  RECORD_BIN=<record-manager 경로> python3 -m unittest managers/record/test_compare.py
RECORD_BIN이 없으면 건너뛴다(dev/check.sh가 빌드 뒤 넣어 줌)
"""
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
EXPECTED = os.path.join(HERE, "test", "logger_expected.jsonl")
BIN = os.environ.get("RECORD_BIN", "")

WORKER = {"event": "worker_status", "data": {
    "id": "cam1", "type": "cam", "desired_state": "running", "actual_state": "running", "pid": 101,
    "restart_count": 0, "last_error": None, "config_version": 3, "applied_version": 3,
    "metrics": {"fps": 29.97, "small": 1e-05, "big": 1e20, "whole": 3.0, "n": 123456789012, "ok": True},
    "resources": {"cpu": 0.1, "rss_kb": 2048}, "note": "한글 \"따옴표\" \\ 탭\t끝"}}
SCRIPT = [
    (WORKER, 0.1),
    (WORKER, 0.1),                                            # 같은 상태, 간격 안: 안 남김
    ({"event": "command", "data": {"t_ms": 5, "client": "rest", "op": "start", "id": "cam1", "ok": True, "from": "screen"}}, 0.1),
    ({"event": "system", "data": {"clients": {"rest": True, "logger": True}, "access_mode": "operation"}}, 0.1),
    ({"event": "system", "data": {"clients": {"rest": True, "logger": True}, "access_mode": "operation"}}, 0.1),
    ({"event": "other", "data": {}}, 0.1),
    (dict(WORKER, data=dict(WORKER["data"], actual_state="stopping")), 0.6),  # 바뀜: 바로
    (dict(WORKER, data=dict(WORKER["data"], actual_state="stopping")), 0.1),  # 간격(0.3) 지남: 남김
    ({"event": "worker_removed", "data": {"id": "cam1"}}, 0.1),
    ({"event": "command", "data": {"op": "set", "args": [1, 2.5, None, {"k": []}], "ok": False,
                                    "error": {"code": "bad_request", "message": "x"}}}, 0.2),
]


class FakeManager:
    """구독을 받아 붙으면 정해진 메시지를 정해진 간격으로 보낸다"""

    def __init__(self, path, clients=1):
        self.path, self.clients = path, clients
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(4)
        self.conns = []
        self.done = threading.Event()
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while len(self.conns) < self.clients:
            c, _ = self.srv.accept()
            f = c.makefile("rb")
            req = json.loads(f.readline())
            assert req["op"] == "subscribe", req
            c.sendall(b'{"ok":true}\n')
            self.conns.append(c)
        time.sleep(0.3)
        for msg, gap in SCRIPT:
            line = (json.dumps(msg, ensure_ascii=False) + "\n").encode()
            for c in self.conns:
                c.sendall(line)
            time.sleep(gap)
        self.done.set()

    def close(self):
        for c in self.conns + [self.srv]:
            c.close()


def lines_of(folder):
    out = []
    for name in sorted(os.listdir(folder)):
        if name.endswith(".jsonl"):
            with open(os.path.join(folder, name), encoding="utf-8") as f:
                out.extend(l.rstrip("\n") for l in f if l.strip())
    return out


def strip_time(line):
    """앞의 "t":"..."와 "t_mono_ns":n을 뗀다(그 뒤는 글자 그대로 비교)"""
    line = re.sub(r'^\{"t":"[^"]*",', "{", line)
    return re.sub(r'^\{"t_mono_ns":\d+,', "{", line)


@unittest.skipUnless(BIN, "RECORD_BIN not set")
class TestSameAsLogger(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="rec-cmp-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_same_lines(self):
        sock = os.path.join(self.tmp, "pm.sock")
        fake = FakeManager(sock)
        out = os.path.join(self.tmp, "rec")
        env = dict(os.environ)
        env.pop("WORKSPACE_DATA", None)
        cfg = os.path.join(self.tmp, "rec.json")
        with open(cfg, "w") as f:
            json.dump({"id": "record-manager", "config_version": 1,
                       "config": {"record": {"dir": out, "manager_socket": sock, "interval_s": 0.3, "stopped_interval_s": 0.3}}}, f)
        cpp = subprocess.Popen([BIN, "--config", cfg], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            self.assertTrue(fake.done.wait(15), "did not subscribe")
            time.sleep(0.3)
        finally:
            cpp.send_signal(signal.SIGTERM)
            self.assertEqual(cpp.wait(10), 0)
            fake.close()

        lines = lines_of(out)
        for l in lines:
            self.assertRegex(l, r'^\{"t":"[^"]+","t_mono_ns":\d+,"type":')
        with open(EXPECTED, encoding="utf-8") as f:
            expected = [l.rstrip("\n") for l in f if l.strip()]
        self.assertEqual([strip_time(l) for l in lines if '"type":"logger"' not in l], expected)
        self.assertEqual([json.loads(l).get("event") for l in lines if '"type":"logger"' in l],
                         ["start", "connected", "stop"])


if __name__ == "__main__":
    unittest.main()
