"""stream.py 시험(표준 라이브러리만, Linux): 가짜 워커 스트림 소켓 → Hub → 화면 구독자.

    cd rest && python3 -m unittest test_stream -v
"""
import json
import os
import socket
import tempfile
import threading
import time
import unittest

from workspace_rest import stream


class FakeWorker:
    """워커 PUB 자리: 붙은 구독자에게 기준 상태를 먼저 보내고, send()로 변경분을 보낸다."""

    def __init__(self, path):
        self.path = path
        self.conns = []
        self.seq = 0
        self.state = {"vehicle": {"mode": "POSCTL", "roll": 0}}
        self.events = [{"seq": 1, "text": "fc found"}]
        self.lock = threading.Lock()
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(4)
        self.srv.settimeout(0.1)
        self.stop = False
        self.t = threading.Thread(target=self._accept, daemon=True)
        self.t.start()

    def _line(self, msg):
        self.seq += 1
        msg["seq"] = self.seq
        return (json.dumps(msg) + "\n").encode()

    def _accept(self):
        while not self.stop:
            try:
                c, _ = self.srv.accept()
            except (socket.timeout, OSError):
                continue
            with self.lock:
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
            data = self._line(msg)
            for c in list(self.conns):
                try:
                    c.sendall(data)
                except OSError:
                    self.conns.remove(c)

    def close(self):
        self.stop = True
        self.t.join()
        for c in self.conns:
            c.close()
        self.srv.close()
        os.unlink(self.path)


def wait_for(cond, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.02)
    return cond()


class StreamTest(unittest.TestCase):
    def setUp(self):
        stream.RETRY_SEC = 0.1
        stream.IDLE_CLOSE_SEC = 0.3
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "w.sock")
        self.hub = stream.Hub(lambda wid: stream.UnixLineSource(self.path))

    def test_keyframe_merge_and_reconnect(self):
        w = FakeWorker(self.path)
        up, sub = self.hub.subscribe("w", hz=50)
        # 첫 메시지: 기준 상태(워커에 붙은 뒤)
        msg = None
        self.assertTrue(wait_for(lambda: up._up))
        while True:
            msg = sub.take(1.0)
            self.assertIsNotNone(msg)
            if msg.get("stream") == "up":
                break
        self.assertTrue(msg["kf"])
        self.assertEqual(msg["topics"]["vehicle"]["mode"], "POSCTL")
        self.assertEqual(len(msg["events"]), 1)

        # 변경분 여러 개는 합쳐 최신 값만, 이벤트는 모두
        w.send({"vehicle": {"mode": "POSCTL", "roll": 1}})
        w.send({"vehicle": {"mode": "HOLD", "roll": 2}}, [{"seq": 2, "text": "POSCTL -> HOLD"}])
        w.send({"link": {"loss_pct": 1.5}})
        self.assertTrue(wait_for(lambda: up._topics.get("link")))
        msg = sub.take(1.0)
        self.assertFalse(msg["kf"])
        self.assertEqual(msg["topics"]["vehicle"], {"mode": "HOLD", "roll": 2})
        self.assertEqual(msg["topics"]["link"], {"loss_pct": 1.5})
        self.assertEqual([e["seq"] for e in msg["events"]], [2])

        # 두 번째 화면: 위쪽 연결은 그대로 하나, 바로 최신 기준 상태
        up2, sub2 = self.hub.subscribe("w", hz=50)
        self.assertIs(up, up2)
        msg2 = sub2.take(1.0)
        self.assertTrue(msg2["kf"])
        self.assertEqual(msg2["topics"]["vehicle"]["mode"], "HOLD")
        self.assertEqual([e["seq"] for e in msg2["events"]], [1, 2])
        self.assertEqual(len(w.conns), 1)

        # 워커가 다시 시작: down을 알리고, 다시 붙으면 새 기준 상태(이벤트 번호도 처음부터)
        w.close()
        self.assertTrue(wait_for(lambda: not up._up))
        msg = sub.take(1.0)
        self.assertEqual(msg.get("stream"), "down")
        w = FakeWorker(self.path)
        self.assertTrue(wait_for(lambda: up._up))
        msg = sub.take(1.0)
        self.assertTrue(msg["kf"])
        self.assertEqual(msg["stream"], "up")
        self.assertEqual([e["seq"] for e in msg["events"]], [1])

        # 화면이 모두 떠나면 위쪽 연결을 닫는다
        up.unsubscribe(sub)
        up.unsubscribe(sub2)
        self.assertTrue(wait_for(lambda: self.hub.stats() == {}, 3.0))
        w.close()

    def test_worker_absent(self):
        up, sub = self.hub.subscribe("w", hz=10)
        msg = sub.take(1.0)
        self.assertTrue(msg["kf"])
        self.assertEqual(msg["stream"], "down")
        up.unsubscribe(sub)

    def test_sse_writer(self):
        w = FakeWorker(self.path)
        up, sub = self.hub.subscribe("w", hz=20)
        r, wsock = socket.socketpair()
        wfile = wsock.makefile("wb")
        stop = []
        t = threading.Thread(target=stream.serve_sse, args=(wfile, up, sub, lambda: bool(stop)))
        t.start()
        r.settimeout(2.0)
        buf = b""
        while b"POSCTL" not in buf:
            buf += r.recv(65536)
        self.assertTrue(buf.startswith(b"data: {"))
        stop.append(1)
        t.join(3.0)
        self.assertFalse(t.is_alive())
        self.assertEqual(self.hub.stats().get("w", 0), 0)
        r.close()
        wfile.close()
        wsock.close()
        w.close()


if __name__ == "__main__":
    unittest.main()
