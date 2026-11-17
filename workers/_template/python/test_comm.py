"""Python 워커 통신 도구 ↔ C++ 워커 통합 시험(design.md 6.6.7). Linux에서, 표준 라이브러리만.

    WORKER_BIN=<example-worker 경로> python3 -m unittest test_comm -v      (이 폴더에서. dev/check.sh가 부른다)

  1) Python → C++: C++ 템플릿 워커 a의 스트림 구독(Subscriber), 명령(CommandClient), 데이터 버스 InOrder 수신(FramebusConsumer)
  2) C++ → Python: Python 쪽 CommandServer·Publisher를 C++ 워커 b(peer = "p")가 구독·명령
WORKER_BIN이 없으면 건너뛴다.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import workspace_worker as ww  # noqa: E402

BIN = os.environ.get("WORKER_BIN", "")


def wait_for(fn, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.05)
    return fn()


@unittest.skipUnless(sys.platform.startswith("linux") and BIN and os.access(BIN, os.X_OK), "needs WORKER_BIN (Linux)")
class Comm(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="ww-comm-")
        for d in ("run/cmd", "run/stream", "run/frames"):
            os.makedirs(os.path.join(self.tmp, d))
        os.environ["WORKSPACE_DATA"] = self.tmp  # 소켓 기본 위치를 시험 폴더로(C++과 Python이 같은 규칙)
        os.environ["XDG_RUNTIME_DIR"] = self.tmp
        os.environ.pop("WORKSPACE_NOTIFY_SOCKET", None)
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def start(self, wid, config):
        path = os.path.join(self.tmp, wid + ".json")
        with open(path, "w") as f:
            json.dump({"id": wid, "config_version": 1, "config": config}, f)
        p = subprocess.Popen([BIN, "--config", path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.procs.append(p)
        self.assertTrue(wait_for(lambda: ww.send_command(ww.default_command_socket(wid), "get_status").get("ok")),
                        "worker %s did not start" % wid)
        return p

    def test_python_to_cpp(self):
        self.start("a", {"source": {"rate_hz": 50, "size": 16}})
        # 구독: 스트림 주제 source
        sub = ww.Subscriber("a")
        sub.start()
        got = {}

        def topics():
            while True:
                u = sub.next()
                if u is None:
                    return "source" in got
                if not u["down"]:
                    got.update(u["topics"])
        self.assertTrue(wait_for(topics, 8), "no stream topic from C++ worker")
        self.assertTrue(sub.connected)
        # 명령: 비동기 요청·응답
        cc = ww.CommandClient("a")
        cc.start()
        rid = cc.send("source.set_rate", {"hz": 20})
        self.assertGreater(rid, 0)
        reply = wait_for(cc.next, 5)
        self.assertIsNotNone(reply)
        self.assertEqual(reply[0], rid)
        self.assertTrue(reply[2]["ok"], reply)
        self.assertEqual(reply[2]["data"]["rate_hz"], 20)
        self.assertEqual(cc.next(), None)
        bad = ww.send_command(ww.default_command_socket("a"), "source.set_rate", {"hz": 0})
        self.assertEqual(bad["error"]["code"], "bad_request")
        # 데이터 버스: 순서대로 전부, 내용(float32 x 16), 반납
        fb = ww.FramebusConsumer(ww.default_socket("a", "frames"), mode="inorder")
        self.assertTrue(wait_for(fb.connect, 5))
        self.assertEqual(fb.format["pixel"], "f32x16")
        ids = []
        end = time.time() + 6
        while len(ids) < 30 and time.time() < end:
            f = fb.next(200)
            if f is fb.DISCONNECTED:
                self.fail("bus disconnected")
            if f is None:
                continue
            self.assertEqual(f.bytes, 16 * 4)
            vals = struct.unpack("<16f", bytes(f.data))
            self.assertTrue(all(v == v for v in vals))  # NaN 아님
            ids.append(f.frame_id)
            f.data.release()
            fb.release(f)
        self.assertGreaterEqual(len(ids), 30)
        self.assertEqual(ids, list(range(ids[0], ids[0] + len(ids))))  # 빠짐 없음(InOrder)
        fb.close()
        sub.stop()
        cc.stop()
        # 상대가 다시 시작하면 구독은 down을 받고 다시 붙는다
        sub2 = ww.Subscriber("a", retry_sec=0.2)
        sub2.start()
        self.assertTrue(wait_for(lambda: sub2.connected, 5))
        self.procs[0].terminate()
        self.procs[0].wait(5)
        self.assertTrue(wait_for(lambda: not sub2.connected, 5))
        self.start("a", {"source": {"rate_hz": 20}})
        self.assertTrue(wait_for(lambda: sub2.connected, 8))
        sub2.stop()

    def test_cpp_to_python(self):
        # Python 워커 p 흉내: 명령 소켓 + 스트림 발행
        rate = {"hz": 5.0}

        def handler(cmd, args):
            if cmd == "get_status":
                return ww.ok_reply({"py": {"rate_hz": rate["hz"]}})
            if cmd == "source.set_rate":
                hz = args.get("hz")
                if not isinstance(hz, (int, float)) or hz <= 0:
                    return ww.error_reply("bad_request", "hz > 0")
                rate["hz"] = float(hz)
                return ww.ok_reply({"rate_hz": rate["hz"]})
            return None
        srv = ww.CommandServer(ww.default_command_socket("p"), handler)
        pub = ww.Publisher(ww.default_stream_socket("p"))
        srv.open()
        pub.open()
        stop = threading.Event()

        def loop():
            seq = 0
            while not stop.is_set():
                srv.serve(0.05)
                pub.pump()
                if pub.subscribers():
                    seq += 1
                    kf = pub.keyframe_requested()
                    pub.publish({"seq": seq, "kf": kf, "t": int(time.time() * 1000),
                                 "topics": {"source": {"rate_hz": rate["hz"], "seq": seq}}, "events": [], "events_next": 0})
        t = threading.Thread(target=loop, daemon=True)
        t.start()
        try:
            # C++ 워커 b: peer = "p" → p를 구독하고 명령을 넘긴다(데이터 버스는 p에 없으니 data_link는 false)
            self.start("b", {"source": {"rate_hz": 5}, "peer": {"peer": "p"}})

            def peer():
                st = ww.send_command(ww.default_command_socket("b"), "get_status")
                return st.get("ok") and st["data"].get("peer", {})
            self.assertTrue(wait_for(lambda: (peer() or {}).get("stream_link"), 8), peer())
            self.assertIn("source", peer().get("peer_topics", {}))
            r = ww.send_command(ww.default_command_socket("b"), "peer.set_rate", {"hz": 12}, timeout=3)
            self.assertTrue(r.get("ok"), r)
            self.assertEqual(r["data"]["rate_hz"], 12)  # 응답은 Python p의 것
            self.assertEqual(rate["hz"], 12)
        finally:
            stop.set()
            t.join(2)
            srv.close()
            pub.close()


if __name__ == "__main__":
    unittest.main()
