"""Manager API 클라이언트. 요청 1건 = 연결 1개, 연결·응답 모두 timeout."""
import json
import socket
import time


class ManagerUnavailable(Exception):
    pass


class ManagerClient:
    def __init__(self, path, connect_timeout=1.0, response_timeout=2.0, max_bytes=65536):
        self.path = path
        self.connect_timeout = connect_timeout
        self.response_timeout = response_timeout
        self.max_bytes = max_bytes

    def call(self, request):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.settimeout(self.connect_timeout)
            try:
                sock.connect(self.path)
            except OSError as e:
                raise ManagerUnavailable("connect: %s" % e)
            deadline = time.monotonic() + self.response_timeout
            try:
                sock.settimeout(self.response_timeout)
                sock.sendall(json.dumps(request).encode("utf-8") + b"\n")
                buf = b""
                while b"\n" not in buf:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise ManagerUnavailable("response timeout")
                    sock.settimeout(remaining)
                    chunk = sock.recv(4096)
                    if not chunk:
                        raise ManagerUnavailable("connection closed")
                    buf += chunk
                    if len(buf) > self.max_bytes:
                        raise ManagerUnavailable("response too large")
            except socket.timeout:
                raise ManagerUnavailable("response timeout")
            except OSError as e:
                raise ManagerUnavailable(str(e))
        finally:
            sock.close()
        try:
            return json.loads(buf.split(b"\n", 1)[0])
        except ValueError as e:
            raise ManagerUnavailable("bad response: %s" % e)
