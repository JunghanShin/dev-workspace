"""workspace-rest 진입점: python3 -m workspace_rest

실행 설정은 환경 변수(workspace-start.sh가 <데이터>/config/workspace.env에서 읽어 넘김)로 받는다. 인자로 덮어쓸 수 있다.
0.0.0.0 바인딩: 제어망 IP가 바뀌어도 재listen 불필요. 접근 범위는 방화벽으로 제한 (design.md 10.3).
"""
import argparse
import json
import logging
import os
import re
import signal
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import productbuild, reads, stream, ui
from .client import ManagerClient, ManagerUnavailable

log = logging.getLogger("workspace.rest")

MAX_BODY = 65536

ERROR_STATUS = {
    "bad_request": 400,
    "forbidden": 403,
    "not_found": 404,
    "conflict": 409,
    "internal": 500,
}

_ID = r"(?P<id>[^/]+)"
_NAME = r"(?P<name>[a-z0-9][a-z0-9_-]{0,31})"
# (method, path, op, 성공 코드, 요청 본문 사용)
ROUTES = [
    ("GET", r"/system", "system", 200, None),
    ("GET", r"/workers", "list", 200, None),
    ("GET", r"/workers/" + _ID, "get", 200, None),
    ("GET", r"/workers/" + _ID + r"/config", "get_config", 200, None),
    # 저장: 사용 중 프리셋 파일(워커 폴더, 깃)에 기록하고 적용. apply: 저장하지 않고 이번 부팅 동안만 적용(임시)
    ("PUT", r"/workers/" + _ID + r"/config", "set_config", 202, "config"),
    ("POST", r"/workers/" + _ID + r"/config/apply", "set_config", 202, "config", {"save": False}),
    # 프리셋(워커 폴더 presets/<이름>.json, design.md 6.3.1)
    ("POST", r"/workers/" + _ID + r"/presets", "save_preset", 202, "preset"),
    ("POST", r"/workers/" + _ID + r"/presets/" + _NAME + r"/use", "use_preset", 202, None),
    ("PUT", r"/workers/" + _ID + r"/presets/" + _NAME, "rename_preset", 202, "preset"),
    ("DELETE", r"/workers/" + _ID + r"/presets/" + _NAME, "delete_preset", 202, None),
    # 설정 폼 스키마(워커 type 선언의 config_schema, design.md 6.10)
    ("GET", r"/workers/" + _ID + r"/schema", "get_schema", 200, None),
    # 입출력 정의(계약, design.md 6.6.7.1): 공통·워커·역할·쓰는 정의. 화면 담당자가 보드에서 바로 받음
    ("GET", r"/workers/" + _ID + r"/io", "get_io", 200, None),
    # 부팅 정책. 본문 {"autostart": true|false}
    ("PUT", r"/workers/" + _ID + r"/autostart", "set_autostart", 202, "autostart"),
    ("POST", r"/workers/" + _ID + r"/start", "start", 202, None),
    ("POST", r"/workers/" + _ID + r"/stop", "stop", 202, None),
]
ROUTES = [(r[0], re.compile("^" + r[1] + "/?$"), r[2], r[3], r[4], r[5] if len(r) > 5 else {}) for r in ROUTES]

# 페이로드 명령: Manager를 거치지 않고 워커 명령 소켓으로 보낸다 (design.md 6.4, 9.6)
COMMAND_ROUTE = re.compile("^/workers/" + _ID + "/command/?$")
COMMAND_STATUS = {"bad_request": 400, "unavailable": 503, "timeout": 504, "internal": 500}

# 텔레메트리 스트림: 워커 스트림 소켓 → SSE (design.md 9.7, stream.py). ?hz=N 화면당 초당 최대 전송(1~50, 기본 10)
STREAM_ROUTE = re.compile("^/workers/" + _ID + "/stream/?$")
STREAM_HZ_DEFAULT = 10
STREAM_HZ_MAX = 50


def worker_command(path, request, connect_timeout=1.0, response_timeout=1.0):
    """워커 명령 소켓에 JSON 한 줄을 보내고 응답 한 줄을 받는다. 실패하면 OSError·ValueError."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.settimeout(connect_timeout)
        sock.connect(path)
        sock.settimeout(response_timeout)
        sock.sendall(json.dumps(request).encode("utf-8") + b"\n")
        buf = b""
        while b"\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
            if len(buf) > MAX_BODY:
                raise ValueError("response too large")
    finally:
        sock.close()
    return json.loads(buf.split(b"\n", 1)[0])


class Handler(BaseHTTPRequestHandler):
    server_version = "workspace-rest/0.1"
    timeout = 5  # 느린 HTTP 클라이언트가 스레드를 붙잡지 못하게
    client = None
    data_dir = ""
    command_timeout = 3.0  # 워커 명령 응답 대기(초). 장치 응답(ACK 등)을 기다리는 명령이 있다
    hub = None             # stream.Hub (main에서 만든다)
    build_job = None       # productbuild.Job (main에서 만든다, 개발 모드)

    def log_message(self, fmt, *args):
        log.info("%s %s", self.address_string(), fmt % args)

    def _send(self, status, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_raw(self, status, data, ctype):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _ui(self, method, path):
        """화면 제공(ui.py). 처리했으면 True. /api/board/<경로>는 self.path에서 접두어만 떼고 False"""
        if path.startswith("/api/board/"):
            self.path = self.path[len("/api/board"):]
            return False
        if method != "GET":
            return False
        query = self.path.split("?", 1)[1] if "?" in self.path else ""
        if path.startswith("/api/remote/"):
            from urllib.parse import parse_qs
            params = {k: v[-1] for k, v in parse_qs(query).items()}
            try:
                status, obj = ui.remote(path[len("/api/remote/"):], params, self.data_dir)
            except KeyError:
                status, obj = 404, {"error": "unknown remote command"}
            except reads.BadRequest as e:
                status, obj = 400, {"error": str(e)}
            except Exception as e:  # 읽기 실패가 REST를 멈추지 않게
                log.exception("ui read %s failed", path)
                status, obj = 200, {"ok": False, "error": str(e)}
            self._send(status, obj)
            return True
        if path == "/api/panels":
            self._send(200, ui.panels())
            return True
        if path == "/api/ui":
            self._send(200, ui.ui_info(self.headers.get("Host"), self.data_dir, self.product))
            return True
        if path in ("/", "/index.html"):
            if not ui.available():
                self._error(404, "not_found", "UI files not found in %s" % ui.WEB)
                return True
            path = "/shell.html"
        found = ui.static_file(path)
        if found is None:
            return False
        self._send_raw(200, found[0], found[1])
        return True

    def _error(self, status, code, message):
        self._send(status, {"error": {"code": code, "message": message}})

    def _read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_BODY:
            raise ValueError("body too large")
        raw = self.rfile.read(length) if length else b""
        return json.loads(raw) if raw else None

    def _health(self):
        try:
            resp = self.client.call({"op": "ping"})
            manager = "ok" if resp.get("ok") else "error"
        except ManagerUnavailable:
            manager = "unreachable"
        self._send(200, {"rest": "ok", "manager": manager})

    def _product_build(self, method, path):
        """화면의 제품 빌드(productbuild.py). 개발 모드에서만"""
        job = self.build_job
        sub = path[len("/system/product-build"):]
        if sub == "" and method == "GET":
            self._send(200, job.status())
        elif sub == "" and method == "POST":
            try:
                body = self._read_body() or {}
            except ValueError as e:
                self._error(400, "bad_request", "invalid JSON body: %s" % e)
                return
            profile = body.get("profile") if isinstance(body, dict) else None
            if not isinstance(profile, str) or not re.match(r"^[A-Za-z0-9_-]{1,64}$", profile):
                self._error(400, "bad_request", 'body must be {"profile": "<name>"}')
                return
            power = body.get("power_mode")
            if power is not None and (not isinstance(power, str) or not re.match(r"^[A-Za-z0-9_]{1,32}$", power)):
                self._error(400, "bad_request", "power_mode must be a mode name or keep")
                return
            try:
                self._send(202, job.start(profile, power))
            except productbuild.Busy as e:
                self._error(409, "conflict", str(e))
        elif sub == "" and method == "DELETE":
            self._send(200, {"cancelled": job.cancel()})
        elif sub == "/check" and method == "GET":
            from urllib.parse import parse_qs
            q = parse_qs(self.path.split("?", 1)[1]) if "?" in self.path else {}
            profile = (q.get("profile") or [""])[-1]
            if not re.match(r"^[A-Za-z0-9_-]{1,64}$", profile):
                self._error(400, "bad_request", "profile is required")
                return
            self._send(200, job.check(profile))
        elif sub == "/download" and method == "GET":
            last = job.last()
            if not last:
                self._error(404, "not_found", "no product bundle yet")
                return
            productbuild.send_file(self, last["path"])
        elif sub in ("", "/check", "/download"):
            self._error(405, "method_not_allowed", "%s not allowed on %s" % (method, path))
        else:
            self._error(404, "not_found", "no route %s" % path)

    def _command(self, worker_id):
        """POST /workers/{id}/command  본문 {"cmd": ..., "args": {...}} → 워커 명령 소켓"""
        try:
            body = self._read_body()
        except ValueError as e:
            self._error(400, "bad_request", "invalid JSON body: %s" % e)
            return
        if not isinstance(body, dict) or not isinstance(body.get("cmd"), str):
            self._error(400, "bad_request", 'body must be {"cmd": "...", "args": {...}}')
            return
        # 소켓 위치는 워커 설정(command_socket), 없으면 기본 위치 (design.md 9.6)
        try:
            resp = self.client.call({"op": "get_config", "id": worker_id})
        except ManagerUnavailable as e:
            self._error(503, "unavailable", "manager unavailable: %s" % e)
            return
        if not resp.get("ok"):
            err = resp.get("error", {})
            self._send(ERROR_STATUS.get(err.get("code"), 500), {"error": err})
            return
        config = (resp.get("data") or {}).get("config") or {}
        path = config.get("command_socket") or os.path.join(
            self.data_dir, "run", "cmd", worker_id + ".sock")
        request = {"cmd": body["cmd"], "args": body.get("args") or {}}
        try:
            reply = worker_command(path, request, response_timeout=self.command_timeout)
        except FileNotFoundError:
            self._error(503, "unavailable", "worker %s is not running (no command socket)" % worker_id)
            return
        except socket.timeout:
            self._error(504, "timeout", "worker %s did not answer" % worker_id)
            return
        except (OSError, ValueError) as e:
            self._error(503, "unavailable", "worker %s: %s" % (worker_id, e))
            return
        if reply.get("ok"):
            self._send(200, reply.get("data"))
        else:
            err = reply.get("error", {})
            self._send(COMMAND_STATUS.get(err.get("code"), 500), {"error": err})

    def _stream(self, worker_id):
        """GET /workers/{id}/stream?hz=N  텔레메트리 변경분(SSE). 첫 메시지는 기준 상태(kf)"""
        from urllib.parse import parse_qs
        query = self.path.split("?", 1)[1] if "?" in self.path else ""
        try:
            hz = float((parse_qs(query).get("hz") or [STREAM_HZ_DEFAULT])[-1])
        except ValueError:
            self._error(400, "bad_request", "hz must be a number")
            return
        hz = min(max(hz, 0.2), STREAM_HZ_MAX)
        # 없는 워커는 바로 404(있는 워커가 꺼져 있으면 스트림은 열고 stream:"down"으로 알린다)
        try:
            resp = self.client.call({"op": "get", "id": worker_id})
        except ManagerUnavailable as e:
            self._error(503, "unavailable", "manager unavailable: %s" % e)
            return
        if not resp.get("ok"):
            err = resp.get("error", {})
            self._send(ERROR_STATUS.get(err.get("code"), 500), {"error": err})
            return
        up, sub = self.hub.subscribe(worker_id, hz)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        self.wfile.write(b"retry: 2000\n\n")
        stream.serve_sse(self.wfile, up, sub)
        self.close_connection = True

    @classmethod
    def stream_socket_path(cls, worker_id):
        """워커 스트림 소켓 위치: 설정 stream_socket, 없으면 기본 위치. 없는 워커면 LookupError"""
        try:
            resp = cls.client.call({"op": "get_config", "id": worker_id})
        except ManagerUnavailable as e:
            raise LookupError("manager unavailable: %s" % e)
        if not resp.get("ok"):
            raise LookupError(str(resp.get("error")))
        config = (resp.get("data") or {}).get("config") or {}
        return config.get("stream_socket") or os.path.join(
            cls.data_dir, "run", "stream", worker_id + ".sock")

    def _read(self, name, match):
        """읽기 전용 기능(reads.py): 로그, 명령 이력, 프로세스, 전원 모드, 지난 지표"""
        from urllib.parse import parse_qs
        query = self.path.split("?", 1)[1] if "?" in self.path else ""
        params = {k: v[-1] for k, v in parse_qs(query).items()}
        try:
            status, obj = reads.handle(name, match, params, self.data_dir)
        except reads.BadRequest as e:
            self._error(400, "bad_request", str(e))
            return
        except Exception as e:  # 읽기 실패가 REST를 멈추지 않게
            log.exception("read %s failed", name)
            self._error(500, "internal", "%s: %s" % (name, e))
            return
        self._send(status, obj)

    # ---- 제품 모드 스위치(workspace.env / 서비스 환경, design.md 6.4·11.4) ----
    token = ""        # WORKSPACE_REST_TOKEN: 비어 있지 않으면 /health·화면 파일 말고 모두 토큰 필요
    ui_enabled = True  # WORKSPACE_UI=0이면 보드가 화면을 내주지 않는다(별도 제어 UI가 REST만 씀)
    product = False   # WORKSPACE_PRODUCT=1: 제품 빌드 같은 개발 기능 없음

    _STATIC = (".html", ".js", ".css", ".svg", ".png", ".ico", ".woff2", ".json")

    def _authorized(self, path):
        if not self.token or path.rstrip("/") in ("/health", "/api/board/health"):
            return True
        if self.ui_enabled and (path in ("/", "/index.html") or
                                (not path.startswith(("/api/", "/workers", "/system", "/logs", "/records", "/history"))
                                 and path.endswith(self._STATIC))):
            return True  # 화면 파일 자체에는 데이터가 없다. 데이터는 /api·REST 경로에서 토큰을 본다
        import hmac
        from urllib.parse import parse_qs
        got = ""
        auth = self.headers.get("Authorization") or ""
        if auth.startswith("Bearer "):
            got = auth[7:].strip()
        if not got:
            for part in (self.headers.get("Cookie") or "").split(";"):
                k, _, v = part.strip().partition("=")
                if k == "workspace_token":
                    got = v
        if not got and "?" in self.path:  # EventSource는 헤더를 못 붙인다(같은 출처면 쿠키로도 됨)
            got = (parse_qs(self.path.split("?", 1)[1]).get("token") or [""])[-1]
        return hmac.compare_digest(got.encode(), self.token.encode())

    def _route(self, method):
        path = self.path.split("?", 1)[0]
        if not self._authorized(path):
            self._error(401, "unauthorized", "token required (Authorization: Bearer <token>)")
            return
        if not self.ui_enabled and method == "GET" and (path in ("/", "/index.html") or path.startswith("/api/")
                                                        and not path.startswith("/api/board/")):
            self._error(404, "not_found", "board UI disabled (WORKSPACE_UI=0)")
            return
        if self._ui(method, path):
            return
        path = self.path.split("?", 1)[0]  # /api/board/ 접두어를 뗐을 수 있다
        if method == "GET" and path.rstrip("/") == "/health":
            self._health()
            return
        if self.product and path.startswith("/system/product-build"):
            self._error(404, "not_found", "not available in product mode")
            return
        if path.rstrip("/").startswith("/system/product-build"):
            self._product_build(method, path.rstrip("/"))
            return
        if method == "GET":
            for pattern, name in reads.ROUTES:
                match = pattern.match(path)
                if match:
                    self._read(name, match)
                    return
        stream_match = STREAM_ROUTE.match(path)
        if stream_match:
            if method != "GET":
                self._error(405, "method_not_allowed", "%s not allowed on %s" % (method, path))
            else:
                self._stream(stream_match.group("id"))
            return
        cmd_match = COMMAND_ROUTE.match(path)
        if cmd_match:
            if method != "POST":
                self._error(405, "method_not_allowed", "%s not allowed on %s" % (method, path))
            else:
                self._command(cmd_match.group("id"))
            return
        matched_path = False
        for m, pattern, op, ok_status, body_key, extra in ROUTES:
            match = pattern.match(path)
            if not match:
                continue
            matched_path = True
            if m != method:
                continue
            request = dict(extra, op=op)
            for key in ("id", "name"):
                if key in match.groupdict():
                    request[key] = match.group(key)
            if body_key:
                try:
                    request[body_key] = self._read_body()
                except ValueError as e:
                    self._error(400, "bad_request", "invalid JSON body: %s" % e)
                    return
            try:
                resp = self.client.call(request)
            except ManagerUnavailable as e:
                self._error(503, "unavailable", "manager unavailable: %s" % e)
                return
            if resp.get("ok"):
                self._send(ok_status, resp.get("data"))
            else:
                err = resp.get("error", {})
                self._send(ERROR_STATUS.get(err.get("code"), 500), {"error": err})
            return
        if matched_path:
            self._error(405, "method_not_allowed", "%s not allowed on %s" % (method, path))
        else:
            self._error(404, "not_found", "no route %s" % path)

    def do_GET(self):
        self._route("GET")

    def do_POST(self):
        self._route("POST")

    def do_PUT(self):
        self._route("PUT")

    def do_DELETE(self):
        self._route("DELETE")


def main():
    env = os.environ.get
    ap = argparse.ArgumentParser(prog="workspace-rest")
    data = env("WORKSPACE_DATA", "")
    ap.add_argument("--manager-socket", default=env(
        "WORKSPACE_MANAGER_SOCKET", os.path.join(data, "run", "manager-rest.sock")))
    ap.add_argument("--bind", default=env("WORKSPACE_REST_BIND", "0.0.0.0"))
    ap.add_argument("--port", type=int, default=int(env("WORKSPACE_REST_PORT", "8080")))
    ap.add_argument("--connect-timeout", type=float,
                    default=float(env("WORKSPACE_REST_CONNECT_TIMEOUT", "1.0")))
    ap.add_argument("--command-timeout", type=float,
                    default=float(env("WORKSPACE_REST_COMMAND_TIMEOUT", "3.0")))
    ap.add_argument("--response-timeout", type=float,
                    default=float(env("WORKSPACE_REST_RESPONSE_TIMEOUT", "2.0")))
    ap.add_argument("--log-level", default=env("WORKSPACE_LOG_LEVEL", "INFO"))
    args = ap.parse_args()
    logging.basicConfig(level=args.log_level,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    Handler.data_dir = data
    Handler.command_timeout = args.command_timeout
    Handler.token = env("WORKSPACE_REST_TOKEN", "")
    Handler.ui_enabled = env("WORKSPACE_UI", "1") != "0"
    Handler.product = env("WORKSPACE_PRODUCT", "0") == "1"
    Handler.client = ManagerClient(args.manager_socket, args.connect_timeout,
                                   args.response_timeout)
    Handler.build_job = productbuild.Job(data)
    Handler.hub = stream.Hub(lambda wid: stream.UnixLineSource(Handler.stream_socket_path(wid)))
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    server.daemon_threads = True

    def _stop(_sig, _frame):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, _stop)
    log.info("listening on %s:%d, manager %s%s%s%s", args.bind, args.port, args.manager_socket,
             ", token required" if Handler.token else "", "" if Handler.ui_enabled else ", UI off",
             ", product mode" if Handler.product else "")
    try:
        server.serve_forever()
    except (SystemExit, KeyboardInterrupt):
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
