"""nginx 앞단 수락 시험(docs/gateway.md 2장): 시작 셸과 같은 설정(gateway/nginx/render.sh)으로 nginx를 띄우고,
그 뒤에 Unix 소켓만 여는 C++ 게이트웨이(WORKSPACE_GATEWAY_TCP=0)를 둔다. 가짜 Manager·워커는 test_rest_accept의 것.

    cd rest && REST_BIN=<게이트웨이> python3 -m unittest test_nginx_front -v

확인: 화면 파일은 nginx가 직접(게이트웨이가 꺼져도 나옴), 없는 경로(API·워커 패널 파일)는 게이트웨이로, Host 전달,
스트림(SSE)은 버퍼링 없이 바로, 본문 상한(64 KiB) 넘으면 413, 명령 기록의 보낸 쪽 주소는 nginx가 넣은 값.
HTTPS(docs/https.md): 보드 자체 서명 인증서(make-cert.sh)로 TLS·이름 확인, 화면·API·스트림, 평문 → HTTPS 넘김(보드 안 127.0.0.1은 그대로).
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
MAKE_CERT = os.path.join(HERE, "..", "gateway", "nginx", "make-cert.sh")
APPLY = os.path.join(HERE, "..", "gateway", "nginx", "apply.sh")
NGINX = shutil.which("nginx") or ("/usr/sbin/nginx" if os.path.exists("/usr/sbin/nginx") else None)


class FakeMedia:
    """가짜 MediaMTX WebRTC(docs/media-proxy.md): 받은 (메서드, 경로, 본문)을 남기고 그대로 돌려준다"""

    def __init__(self):
        import http.server
        import threading
        self.got = []
        me = self

        class H(http.server.BaseHTTPRequestHandler):
            def _reply(self):
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n) if n else b""
                me.got.append((self.command, self.path, body))
                out = json.dumps({"method": self.command, "path": self.path, "body": body.decode()}).encode()
                self.send_response(201 if self.command == "POST" else 200)
                if self.command == "POST":  # MediaMTX WHEP: 세션 주소는 절대 경로
                    self.send_header("Location", self.path.rstrip("/") + "/abc123")
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(out)))
                self.end_headers()
                self.wfile.write(out)

            do_GET = do_POST = do_PATCH = do_DELETE = _reply

            def log_message(self, *a):
                pass

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()


@unittest.skipUnless(t.REST_BIN and NGINX, "C++ 게이트웨이(REST_BIN)와 nginx가 있어야 함")
class NginxFront(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        t.setUpModule()  # 가짜 Manager·워커, 시험용 워크스페이스(ui/web, 워커 패널)
        defs = os.path.join(t.T["root"], "install", "config")  # 설정 탭 칸 정의(영상 주소 틀 바꾸기 시험)
        os.makedirs(defs, exist_ok=True)
        shutil.copy(os.path.join(HERE, "..", "install", "config", "workspace-settings.json"), defs)
        cls.stream = t.FakeStreamWorker(os.path.join(t.T["data"], "run", "stream", "s1.sock"))
        cls.sock = os.path.join(t.T["tmp"], "gw-front.sock")
        cls.gw = t.Server({"WORKSPACE_GATEWAY_SOCKET": cls.sock, "WORKSPACE_GATEWAY_TCP": "0"}, name="gw-front",
                          unix_only=cls.sock)
        cls.port = t.free_port()
        prefix = os.path.join(t.T["tmp"], "nginx")
        conf = os.path.join(prefix, "nginx.conf")
        os.makedirs(prefix, exist_ok=True)
        cls.media = FakeMedia()
        subprocess.check_call(["bash", RENDER, conf, prefix, cls.sock, "127.0.0.1:%d" % cls.port, t.T["root"], "1"],
                              env=dict(os.environ, RENDER_MEDIA="127.0.0.1:%d" % cls.media.port))
        subprocess.check_call([NGINX, "-t", "-q", "-p", prefix, "-c", conf, "-g", "daemon off;"])
        cls.ng_log = open(os.path.join(prefix, "nginx.log"), "wb")
        cls.ng = subprocess.Popen([NGINX, "-p", prefix, "-c", conf, "-g", "daemon off;"], stdout=cls.ng_log, stderr=cls.ng_log)
        assert t.wait_for(lambda: cls._open(cls.port), 5, 0.05), "nginx did not start"
        # 워커 명령(계약 없는 가짜 워커)을 보는데, 워크스페이스는 늘 개발 모드(docs/modes.md)

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
        cls.media.close()
        t.tearDownModule()

    def test_media_proxy(self):
        """영상 넘기기(docs/media-proxy.md): /media/<경로>/ → MediaMTX /<경로>/, 신호(WHEP POST)도. 기본 영상 주소는 같은 주소"""
        st, h, data = self.req("GET", "/media/siyi/")
        self.assertEqual((st, json.loads(data)["path"]), (200, "/siyi/"))
        st, h, data = self.req("POST", "/media/siyi/whep", raw=b"v=0 offer", headers={"Content-Type": "application/sdp"})
        self.assertEqual((st, json.loads(data)["path"], json.loads(data)["body"]), (201, "/siyi/whep", "v=0 offer"))
        self.assertEqual(h["location"], "/media/siyi/whep/abc123")  # 세션 주소도 /media/ 아래(PATCH·DELETE가 같은 길로)
        st, h, data = self.req("DELETE", "/media/siyi/whep/abc123")
        self.assertEqual((st, json.loads(data)["path"]), (200, "/siyi/whep/abc123"))
        st, h, data = self.req("GET", "/api/ui", headers={"Host": "board.test:8080"})
        # nginx 뒤(게이트웨이 TCP 안 엶): 같은 주소의 /media/를 완전한 주소로(별도 UI가 읽어도 이 보드로)
        self.assertEqual(json.loads(data)["media"]["url"], "http://board.test:8080/media/{path}/")
        st, h, data = self.req("GET", "/api/ui", headers={"Host": "bo_ard:8080"})
        self.assertEqual(json.loads(data)["media"]["url"], "/media/{path}/")  # 이상한 Host는 상대 그대로

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
        self.assertEqual(h.get("cache-control"), "no-cache")  # 업데이트 뒤 옛 화면 파일 방지(ETag로 확인)
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
        # 기본 영상 주소는 상대 경로(/media/)라 {host}가 없다: 틀을 잠깐 {host}로 바꿔(설정 탭, 바로 적용) Host 전달을 본다
        put = lambda v: self.req("PUT", "/api/board/workspace/settings", body={"WORKSPACE_UI_MEDIA_URL": v},
                                 headers={"Content-Type": "application/json"})
        self.assertEqual(put("http://{host}:8889/{path}")[0], 200)
        try:
            st, _, data = self.req("GET", "/api/ui", headers={"Host": "board.test:8080"})
            self.assertEqual(st, 200)
            self.assertIn("board.test", json.loads(data)["media"]["url"])  # 영상 주소에 브라우저가 접속한 주소
        finally:
            self.assertEqual(put(None)[0], 200)
        self.assertTrue(json.loads(self.req("GET", "/api/ui")[2])["media"]["url"].endswith("/media/{path}/"))  # 비우면 자동

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

    def test_streams_not_buffered(self):
        """/streams(여러 워커를 한 연결로, docs/stream-mux.md)도 버퍼링 없이"""
        sse = t.SSE(self.port, "/api/board/streams?ids=s1:20")
        try:
            self.assertEqual(sse.status, 200)
            first = sse.wait_msg(lambda m: m.get("stream") == "up", 5)  # 위쪽이 아직이면 down 줄이 먼저
            self.assertTrue(first and first.get("kf") and first.get("worker") == "s1", first)
            t0 = time.time()
            self.stream.send({"device": {"mode": "MUX", "roll": 2}})
            got = None
            while time.time() - t0 < 3:
                m = sse.msg(3)
                if m and m.get("topics", {}).get("device", {}).get("mode") == "MUX":
                    got = time.time() - t0
                    break
            self.assertIsNotNone(got, "update did not arrive")
            self.assertLess(got, 1.0)
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


@unittest.skipUnless(t.REST_BIN and NGINX and shutil.which("openssl"), "C++ 게이트웨이(REST_BIN)·nginx·openssl이 있어야 함")
class NginxHttps(unittest.TestCase):
    """HTTPS(docs/https.md): 시작 셸과 같은 순서(make-cert.sh → render.sh에 RENDER_TLS_*)"""

    @classmethod
    def setUpClass(cls):
        t.setUpModule()
        cls.stream = t.FakeStreamWorker(os.path.join(t.T["data"], "run", "stream", "s2.sock"))
        cls.sock = os.path.join(t.T["tmp"], "gw-tls.sock")
        cls.gw = t.Server({"WORKSPACE_GATEWAY_SOCKET": cls.sock, "WORKSPACE_GATEWAY_TCP": "0"}, name="gw-tls",
                          unix_only=cls.sock)
        cls.port, cls.tport = t.free_port(), t.free_port()
        prefix = os.path.join(t.T["tmp"], "nginx-tls")
        os.makedirs(prefix, exist_ok=True)
        cls.tls = os.path.join(prefix, "tls")
        env = dict(os.environ, CERT_IPS="192.0.2.10")  # 문서용 주소(실제 보드 주소 아님)
        out = subprocess.check_output(["bash", MAKE_CERT, cls.tls], env=env, text=True)
        assert "made" in out, out
        cls.crt = os.path.join(cls.tls, "workspace.crt")
        conf = os.path.join(prefix, "nginx.conf")
        subprocess.check_call(["bash", RENDER, conf, prefix, cls.sock, "127.0.0.1:%d" % cls.port, t.T["root"], "1"],
                              env=dict(os.environ, RENDER_TLS_LISTEN="127.0.0.1:%d" % cls.tport, RENDER_TLS_CERT=cls.crt,
                                       RENDER_TLS_KEY=os.path.join(cls.tls, "workspace.key")))
        subprocess.check_call([NGINX, "-t", "-q", "-p", prefix, "-c", conf, "-g", "daemon off;"])
        cls.ng_log = open(os.path.join(prefix, "nginx.log"), "wb")
        cls.ng = subprocess.Popen([NGINX, "-p", prefix, "-c", conf, "-g", "daemon off;"], stdout=cls.ng_log, stderr=cls.ng_log)
        assert t.wait_for(lambda: NginxFront._open(cls.tport) and NginxFront._open(cls.port), 5, 0.05), "nginx did not start"

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

    def https(self, method, path, body=None, headers=None):
        import http.client
        import ssl
        ctx = ssl.create_default_context(cafile=self.crt)  # 인증서 확인 + 이름(localhost) 확인
        c = http.client.HTTPSConnection("localhost", self.tport, timeout=10, context=ctx)
        try:
            c.request(method, path, body=None if body is None else json.dumps(body).encode(), headers=headers or {})
            r = c.getresponse()
            return r.status, {k.lower(): v for k, v in r.getheaders()}, r.read()
        finally:
            c.close()

    def plain(self, path, source="127.0.0.1"):
        import http.client
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10, source_address=(source, 0))
        try:
            c.request("GET", path)
            r = c.getresponse()
            return r.status, {k.lower(): v for k, v in r.getheaders()}, r.read()
        finally:
            c.close()

    def test_tls_ui_api(self):
        st, _, data = self.https("GET", "/")
        self.assertEqual(st, 200)
        self.assertIn("셸".encode(), data)
        st, _, data = self.https("GET", "/api/board/workers")
        self.assertEqual(st, 200)
        self.assertIn("workers", json.loads(data))

    def test_cert_names(self):
        san = subprocess.check_output(["openssl", "x509", "-in", self.crt, "-noout", "-ext", "subjectAltName"], text=True)
        for name in ("DNS:localhost", "IP Address:127.0.0.1", "IP Address:192.0.2.10"):
            self.assertIn(name, san)
        out = subprocess.check_output(["bash", MAKE_CERT, self.tls], env=dict(os.environ, CERT_IPS="192.0.2.10"), text=True)
        self.assertIn("ok", out)  # 그대로면 다시 만들지 않음

    def test_redirect_and_local_exempt(self):
        st, _, _ = self.plain("/kit.js")  # 보드 안(127.0.0.1): SSH 터널처럼 평문 그대로
        self.assertEqual(st, 200)
        st, h, _ = self.plain("/api/board/workers?x=1", source="127.0.0.2")  # 바깥에서 온 것처럼
        self.assertEqual(st, 308)
        self.assertEqual(h["location"], "https://127.0.0.1:%d/api/board/workers?x=1" % self.tport)

    def test_stream_over_tls(self):
        import http.client
        import ssl
        ctx = ssl.create_default_context(cafile=self.crt)
        c = http.client.HTTPSConnection("localhost", self.tport, timeout=5, context=ctx)
        try:
            c.request("GET", "/workers/s2/stream?hz=20")
            r = c.getresponse()
            self.assertEqual(r.status, 200)
            self.assertIn("text/event-stream", r.getheader("Content-Type", ""))
            line = r.fp.readline()  # 첫 줄이 버퍼링 없이 옴
            self.assertTrue(line.strip(), line)
        finally:
            c.close()


@unittest.skipUnless(t.REST_BIN and NGINX and shutil.which("openssl"), "C++ 게이트웨이(REST_BIN)·nginx·openssl이 있어야 함")
class NginxLive(unittest.TestCase):
    """설정 바로 적용(docs/live-settings.md): 시작 셸과 같은 apply.sh로 nginx를 띄우고, 설정 탭 PUT으로 HTTPS를 바꾸면
    게이트웨이가 apply.sh --reload로 nginx를 다시 읽힌다(nginx는 그대로 같은 프로세스). 로그인은 시작할 때 정한다(NginxLogin)"""
    AUTH = "off"

    @classmethod
    def setUpClass(cls):
        t.setUpModule()
        root = t.T["root"]
        shutil.copytree(os.path.join(HERE, "..", "gateway", "nginx"), os.path.join(root, "gateway", "nginx"), dirs_exist_ok=True)
        defs = os.path.join(root, "install", "config")
        os.makedirs(defs, exist_ok=True)
        shutil.copy(os.path.join(HERE, "..", "install", "config", "workspace-settings.json"), defs)
        cls.data = t.auth_data_dir()  # 사용자 v1·o1·m1 + m1 토큰
        cls.token = t.AUTH_USERS_TOKEN[cls.data]
        cls.port, cls.tport = t.free_port(), t.free_port()
        t.write(os.path.join(cls.data, "config", "workspace.env"),
                "WORKSPACE_REST_BIND=127.0.0.1\nWORKSPACE_REST_PORT=%d\n" % cls.port)
        cls.prefix = os.path.join(cls.data, "run", "nginx")
        cls.sock = os.path.join(cls.data, "run", "gateway.sock")
        cls.media = FakeMedia()
        cls.env = dict(os.environ, RENDER_MEDIA="127.0.0.1:%d" % cls.media.port, CERT_IPS="192.0.2.10",
                       WORKSPACE_NGINX_DIR=cls.prefix, WORKSPACE_AUTH=cls.AUTH)
        out = subprocess.check_output(["bash", APPLY, cls.data, root, cls.sock], env=cls.env, text=True)  # 시작 셸처럼
        assert "config ready" in out, out
        cls.gw = t.Server(t.auth_env(cls.data, WORKSPACE_AUTH=cls.AUTH, WORKSPACE_GATEWAY_SOCKET=cls.sock, WORKSPACE_GATEWAY_TCP="0",
                                     RENDER_MEDIA=cls.env["RENDER_MEDIA"], CERT_IPS="192.0.2.10", WORKSPACE_NGINX_DIR=cls.prefix),
                          name="gw-live", unix_only=cls.sock)
        cls.ng_log = open(os.path.join(cls.prefix, "nginx.log"), "wb")
        conf = os.path.join(cls.prefix, "nginx.conf")
        cls.ng = subprocess.Popen([NGINX, "-p", cls.prefix, "-c", conf, "-g", "daemon off;"], stdout=cls.ng_log, stderr=cls.ng_log)
        assert t.wait_for(lambda: NginxFront._open(cls.port), 5, 0.05), "nginx did not start"

    @classmethod
    def tearDownClass(cls):
        cls.ng.send_signal(signal.SIGTERM)
        try:
            cls.ng.wait(5)
        except subprocess.TimeoutExpired:
            cls.ng.kill()
        cls.ng_log.close()
        cls.gw.stop()
        cls.media.close()
        t.tearDownModule()

    def req(self, method, path, body=None, headers=None, raw=None):
        return NginxFront.req(self, method, path, body, headers, raw)

    def put(self, body):
        st, _, data = self.req("PUT", "/api/board/workspace/settings", body,
                               headers={"Content-Type": "application/json", "Authorization": "Bearer " + self.token})
        return st, json.loads(data)

    def test_live_https(self):
        pid = self.ng.pid
        self.assertEqual(self.req("GET", "/media/cam/")[0], 200)  # 로그인 없음: 열림
        bearer = {"Authorization": "Bearer " + self.token}
        # 로그인은 설정 탭에 없음(docs/modes.md)
        st, obj = self.put({"WORKSPACE_AUTH": "on"})
        self.assertEqual((st, obj["error"]["fields"]), (400, {"WORKSPACE_AUTH": "unknown setting"}))
        # HTTPS 켜기 → 자체 서명 인증서를 만들고 HTTPS 포트를 연다(nginx는 같은 프로세스)
        st, obj = self.put({"WORKSPACE_HTTPS": "on", "WORKSPACE_HTTPS_PORT": self.tport})
        self.assertEqual((st, sorted(obj["applied"]), obj["failed"]), (200, ["WORKSPACE_HTTPS", "WORKSPACE_HTTPS_PORT"], {}), obj)
        self.assertTrue(t.wait_for(lambda: NginxFront._open(self.tport), 5, 0.05))
        import http.client
        import ssl
        ctx = ssl.create_default_context(cafile=os.path.join(self.data, "config", "tls", "workspace.crt"))
        c = http.client.HTTPSConnection("localhost", self.tport, timeout=10, context=ctx)
        c.request("GET", "/api/board/workers")
        self.assertEqual(c.getresponse().status, 200)
        c.close()
        st, _, data = self.req("GET", "/api/board/workspace/settings", headers=bearer)
        fs = {f["key"]: f for g in json.loads(data)["groups"] for f in g["fields"]}
        self.assertEqual((fs["WORKSPACE_HTTPS"]["effective"], fs["WORKSPACE_HTTPS_PORT"]["effective"]), ("on", self.tport))
        # 읽을 수 없는 인증서: 옛 설정 그대로 두고 failed
        st, obj = self.put({"WORKSPACE_HTTPS_CERT": "/no/such.crt", "WORKSPACE_HTTPS_KEY": "/no/such.key"})
        self.assertEqual((st, obj["applied"]), (200, []))
        self.assertIn("not readable", obj["failed"]["WORKSPACE_HTTPS_CERT"])
        self.assertTrue(NginxFront._open(self.tport))
        self.put({"WORKSPACE_HTTPS_CERT": None, "WORKSPACE_HTTPS_KEY": None})
        # 끄기
        st, obj = self.put({"WORKSPACE_HTTPS": "off"})
        self.assertEqual((st, obj["failed"]), (200, {}), obj)
        self.assertTrue(t.wait_for(lambda: not NginxFront._open(self.tport), 5, 0.05))
        self.assertEqual(self.ng.pid, pid)
        self.assertIsNone(self.ng.poll())  # 다시 시작이 아니라 다시 읽기


class NginxLogin(NginxLive):
    """로그인을 넣은 제품(docs/media-auth.md): 시작 셸이 WORKSPACE_AUTH=on을 넘기면 /media/도 로그인한 사람만"""
    AUTH = "on"

    def test_live_https(self):  # 부모 시험은 로그인 없음 전용
        pass

    def test_media_login(self):
        st, h, body = self.req("GET", "/media/cam/")
        self.assertEqual(st, 401)
        self.assertEqual(self.req("GET", "/api/board/workers")[0], 401)
        bearer = {"Authorization": "Bearer " + self.token}
        st, h, data = self.req("GET", "/media/cam/", headers=bearer)
        self.assertEqual((st, json.loads(data)["path"]), (200, "/cam/"))
        st, h, data = self.req("POST", "/api/auth/login", {"name": "v1", "password": t.PASSWORDS["v1"]},
                               headers={"Content-Type": "application/json"})
        self.assertEqual(st, 200)
        cookie = {"Cookie": h["set-cookie"].split(";", 1)[0]}
        # 신호(WHEP POST·DELETE)도: 안쪽 확인은 GET으로 바꿔 묻는다
        st, h, data = self.req("POST", "/media/cam/whep", raw=b"v=0 offer", headers=dict(cookie, **{"Content-Type": "application/sdp"}))
        self.assertEqual((st, json.loads(data)["body"]), (201, "v=0 offer"))
        self.assertEqual(self.req("DELETE", "/media/cam/whep/abc123", headers=cookie)[0], 200)
        self.assertEqual(self.req("DELETE", "/media/cam/whep/abc123")[0], 401)
        self.assertEqual(self.req("GET", "/_media_auth")[0], 404)  # 안쪽 위치는 바깥에서 못 부름

class Render(unittest.TestCase):
    """render.sh가 nginx 빌드에 맞춰 설정을 만드는지(nginx 없이: 가짜 nginx -V). 보드의 Ubuntu 22.04 nginx-light는
    limit_conn 모듈이 없어 설정 점검이 실패했던 일이 있다"""

    def render(self, version_text, extra_env=None):
        import tempfile
        d = tempfile.mkdtemp(prefix="ngr")
        fake = os.path.join(d, "nginx")
        with open(fake, "w") as f:
            f.write("#!/bin/sh\necho '%s' >&2\n" % version_text)
        os.chmod(fake, 0o755)
        out = os.path.join(d, "nginx.conf")
        subprocess.check_call(["bash", RENDER, out, d, "/x/gw.sock", "127.0.0.1:1", "/ws", "1"],
                              env=dict(os.environ, NGINX_BIN=fake, **(extra_env or {})))
        text = open(out).read()
        shutil.rmtree(d, ignore_errors=True)
        return text

    def test_tls_and_redirect(self):
        env = dict(RENDER_TLS_LISTEN="0.0.0.0:443", RENDER_TLS_CERT="/c/w.crt", RENDER_TLS_KEY="/c/w.key")
        full = self.render("configure arguments: --with-http_ssl_module", env)
        self.assertIn("listen 0.0.0.0:443 ssl;", full)
        self.assertIn("ssl_certificate /c/w.crt;", full)
        self.assertIn("return 308 https://$host$request_uri;", full)  # 443이면 포트 안 붙임
        self.assertIn("$remote_addr = 127.0.0.1", full)
        off = self.render("configure arguments: --with-http_ssl_module", dict(env, RENDER_REDIRECT="off"))
        self.assertIn("listen 0.0.0.0:443 ssl;", off)
        self.assertNotIn("return 308", off)
        nossl = self.render("configure arguments: --without-http_limit_conn_module", env)
        self.assertNotIn(" ssl;", nossl)
        none = self.render("configure arguments: --with-http_ssl_module")
        self.assertNotIn(" ssl;", none)
        self.assertFalse([l for l in none.splitlines() if "@TLS@" in l and not l.lstrip().startswith("#")])

    def test_media_auth(self):
        """RENDER_AUTH=on(docs/media-auth.md): /media/에 auth_request, 모듈이 없으면 /media/를 뺀다(열어 두지 않음)"""
        full = self.render("configure arguments: --with-http_auth_request_module", dict(RENDER_AUTH="on"))
        self.assertIn("auth_request /_media_auth;", full)
        self.assertIn("location = /_media_auth", full)
        self.assertIn("proxy_pass http://gateway/api/auth/check;", full)
        self.assertIn("proxy_method GET;", full)
        self.assertIn("location ^~ /media/", full)
        nomod = self.render("configure arguments: --with-http_ssl_module", dict(RENDER_AUTH="on"))
        self.assertNotIn("location ^~ /media/", nomod)
        self.assertNotIn("auth_request", nomod)
        off = self.render("configure arguments: --with-http_auth_request_module")
        self.assertIn("location ^~ /media/", off)
        self.assertNotIn("auth_request", off)

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
            self.assertIn("location ^~ /media/", text)  # 영상 넘기기 기본 켬(docs/media-proxy.md)
            self.assertIn('add_header Cache-Control "no-cache" always;', text)  # 업데이트 뒤 옛 화면 파일 방지


if __name__ == "__main__":
    unittest.main()
