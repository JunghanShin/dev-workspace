"""nginx 앞단 수락 시험(docs/gateway.md 2장): 시작 셸과 같은 설정(gateway/nginx/render.sh)으로 nginx를 띄우고,
그 뒤에 Unix 소켓만 여는 C++ 게이트웨이(WORKSPACE_GATEWAY_TCP=0)를 둔다. 가짜 Manager·워커는 test_rest_accept의 것.

    cd rest && REST_BIN=<게이트웨이> python3 -m unittest test_nginx_front -v

확인: 화면 파일은 nginx가 직접(게이트웨이가 꺼져도 나옴), 없는 경로(API·워커 패널 파일)는 게이트웨이로, Host 전달,
스트림(SSE)은 버퍼링 없이 바로, 본문 상한(64 KiB) 넘으면 413, 명령 기록의 보낸 쪽 주소는 nginx가 넣은 값.
nginx나 REST_BIN이 없으면 건너뛴다.
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import time
import unittest

import test_rest_accept as t

HERE = os.path.dirname(os.path.abspath(__file__))
RENDER = os.path.join(HERE, "..", "gateway", "nginx", "render.sh")
NGINX = shutil.which("nginx") or ("/usr/sbin/nginx" if os.path.exists("/usr/sbin/nginx") else None)


@unittest.skipUnless(t.REST_BIN and NGINX, "C++ 게이트웨이(REST_BIN)와 nginx가 있어야 함")
class NginxFront(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        t.setUpModule()  # 가짜 Manager·워커, 시험용 워크스페이스(ui/web, 워커 패널)
        cls.stream = t.FakeStreamWorker(os.path.join(t.T["data"], "run", "stream", "s1.sock"))
        cls.sock = os.path.join(t.T["tmp"], "gw-front.sock")
        cls.gw = t.Server({"WORKSPACE_GATEWAY_SOCKET": cls.sock, "WORKSPACE_GATEWAY_TCP": "0"}, name="gw-front",
                          unix_only=cls.sock)
        cls.port = t.free_port()
        prefix = os.path.join(t.T["tmp"], "nginx")
        conf = os.path.join(prefix, "nginx.conf")
        os.makedirs(prefix, exist_ok=True)
        subprocess.check_call(["bash", RENDER, conf, prefix, cls.sock, "127.0.0.1:%d" % cls.port, t.T["root"], "1"])
        subprocess.check_call([NGINX, "-t", "-q", "-p", prefix, "-c", conf, "-g", "daemon off;"])
        cls.ng_log = open(os.path.join(prefix, "nginx.log"), "wb")
        cls.ng = subprocess.Popen([NGINX, "-p", prefix, "-c", conf, "-g", "daemon off;"], stdout=cls.ng_log, stderr=cls.ng_log)
        assert t.wait_for(lambda: cls._open(cls.port), 5, 0.05), "nginx did not start"
        # 워커 명령(계약 없는 가짜 워커)을 보므로 개발 모드로(docs/command-path.md 3장)
        import http.client
        c = http.client.HTTPConnection("127.0.0.1", cls.port, timeout=10)
        c.request("PUT", "/api/mode", body=json.dumps({"mode": "development"}).encode())
        assert c.getresponse().status == 200
        c.close()

    @staticmethod
    def _open(port):
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return True
        except OSError:
            return False

    @classmethod
    def tearDownClass(cls):
        cls.ng.send_signal(signal.SIGTERM)
        try:
            cls.ng.wait(5)
        except subprocess.TimeoutExpired:
            cls.ng.kill()
        cls.ng_log.close()
        cls.gw.stop()
        cls.stream.close()
        t.tearDownModule()

    def req(self, method, path, body=None, headers=None, raw=None):
        import http.client
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
            c.request(method, path, body=data, headers=headers or {})
            r = c.getresponse()
            return r.status, {k.lower(): v for k, v in r.getheaders()}, r.read()
        finally:
            c.close()

    def test_static_from_nginx(self):
        st, h, data = self.req("GET", "/")
        self.assertEqual(st, 200)
        self.assertIn("셸".encode(), data)
        st, h, data = self.req("GET", "/kit.js")
        self.assertEqual((st, data), (200, b"export const x = 1;\n"))
        self.assertIn("javascript", h["content-type"])
        self.assertEqual(self.req("GET", "/panels/board/panel.js")[0], 200)  # 공용 패널(ui/web 안)

    def test_api_and_worker_panel_via_gateway(self):
        st, h, data = self.req("GET", "/api/board/workers")
        self.assertEqual(st, 200)
        self.assertIn("workers", json.loads(data))  # 가짜 Manager의 list 응답
        st, _, data = self.req("POST", "/workers/w1/command", {"cmd": "get_status"})
        self.assertEqual(st, 200)
        # 워커 패널 파일: 패널 id(alpha-panel)와 폴더(alpha)가 달라 nginx는 모르고 게이트웨이가 찾음
        st, _, data = self.req("GET", "/panels/alpha-panel/panel.js")
        self.assertEqual((st, data), (200, b"// alpha\n"))
        self.assertEqual(self.req("GET", "/no/such/thing")[0], 404)

    def test_remote_from_nginx(self):
        """명령 기록의 보낸 쪽 주소는 nginx가 넣은 값($remote_addr). 클라이언트가 X-Forwarded-For를 꾸며도 덮인다"""
        st, _, _ = self.req("POST", "/workers/w1/command", {"cmd": "via_nginx"}, headers={"X-Forwarded-For": "203.0.113.9"})
        self.assertEqual(st, 200)
        last = lambda: [l for l in t.log_lines(t.T["data"]) if l["cmd"] == "via_nginx"]
        self.assertTrue(t.wait_for(lambda: last(), 5))
        self.assertEqual(last()[-1]["remote"], "127.0.0.1")

    def test_host_passed(self):
        st, _, data = self.req("GET", "/api/ui", headers={"Host": "board.test:8080"})
        self.assertEqual(st, 200)
        self.assertIn("board.test", json.loads(data)["media"]["url"])  # 영상 주소에 브라우저가 접속한 주소

    def test_body_limit(self):
        st, _, _ = self.req("POST", "/workers/w1/command", raw=b"x" * (70 * 1024),
                            headers={"Content-Type": "application/json"})
        self.assertEqual(st, 413)

    def test_stream_not_buffered(self):
        sse = t.SSE(self.port, "/workers/s1/stream?hz=20")
        try:
            first = sse.msg(3)
            self.assertTrue(first and first.get("kf"), first)
            t0 = time.time()
            self.stream.send({"device": {"mode": "IDLE", "roll": 1}})
            got = None
            while time.time() - t0 < 3:
                m = sse.msg(3)
                if m and m.get("topics", {}).get("device", {}).get("mode") == "IDLE":
                    got = time.time() - t0
                    break
            self.assertIsNotNone(got, "update did not arrive")
            self.assertLess(got, 1.0)  # 버퍼링하면 늦거나 안 옴
        finally:
            sse.close()

    def test_z_static_survives_gateway_down(self):
        """화면 파일은 nginx가 직접 준다: 게이트웨이가 없어도 화면은 열리고 API만 502"""
        self.gw.stop()
        try:
            self.assertEqual(self.req("GET", "/kit.js")[0], 200)
            self.assertEqual(self.req("GET", "/api/board/workers")[0], 502)
        finally:
            type(self).gw = t.Server({"WORKSPACE_GATEWAY_SOCKET": self.sock, "WORKSPACE_GATEWAY_TCP": "0"},
                                     name="gw-front2", unix_only=self.sock)


class Render(unittest.TestCase):
    """render.sh가 nginx 빌드에 맞춰 설정을 만드는지(nginx 없이: 가짜 nginx -V). 보드의 Ubuntu 22.04 nginx-light는
    limit_conn 모듈이 없어 설정 점검이 실패했던 일이 있다"""

    def render(self, version_text):
        import tempfile
        d = tempfile.mkdtemp(prefix="ngr")
        fake = os.path.join(d, "nginx")
        with open(fake, "w") as f:
            f.write("#!/bin/sh\necho '%s' >&2\n" % version_text)
        os.chmod(fake, 0o755)
        out = os.path.join(d, "nginx.conf")
        subprocess.check_call(["bash", RENDER, out, d, "/x/gw.sock", "127.0.0.1:1", "/ws", "1"],
                              env=dict(os.environ, NGINX_BIN=fake))
        text = open(out).read()
        shutil.rmtree(d, ignore_errors=True)
        return text

    def test_with_and_without_limit_conn(self):
        full = self.render("configure arguments: --with-http_ssl_module")
        light = self.render("configure arguments: --without-http_limit_conn_module --without-http_limit_req_module")
        self.assertIn("limit_conn_zone", full)
        self.assertIn("limit_conn perip", full)
        self.assertNotIn("limit_conn_zone", light)
        self.assertNotIn("limit_conn perip", light)
        for text in (full, light):
            body = [l for l in text.splitlines() if not l.lstrip().startswith("#")]  # 머리 설명 주석은 빼고
            self.assertFalse([l for l in body if "@PREFIX@" in l or "@LIMIT" in l or "@UI_LOCATION@" in l])
            self.assertIn("server unix:/x/gw.sock;", text)
            self.assertIn("root /ws/ui/web;", text)


if __name__ == "__main__":
    unittest.main()
