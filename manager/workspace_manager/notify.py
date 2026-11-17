"""워커 → Manager heartbeat 수신 (design.md 9.5).

sd_notify와 같은 방식: Unix SOCK_DGRAM + SO_PASSCRED.
보낸 프로세스의 pid는 커널이 붙여주는 SCM_CREDENTIALS로 확인한다.
"""
import logging
import os
import socket
import struct

log = logging.getLogger("workspace.manager")

_UCRED = struct.Struct("3i")  # pid, uid, gid


def parse_fields(data):
    fields = {}
    for token in data.decode("utf-8", "replace").split():
        key, _, value = token.partition("=")
        fields[key] = value
    return fields


class NotifyListener:
    def __init__(self, path, on_message):
        self.path = path
        self.on_message = on_message
        self.sock = None
        self.loop = None

    def start(self, loop):
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
        sock.bind(self.path)
        os.chmod(self.path, 0o660)
        sock.setblocking(False)
        self.sock = sock
        self.loop = loop
        loop.add_reader(sock.fileno(), self._read)

    def close(self):
        if self.sock is not None:
            self.loop.remove_reader(self.sock.fileno())
            self.sock.close()
            self.sock = None

    def _read(self):
        while True:
            try:
                data, ancdata, _flags, _addr = self.sock.recvmsg(
                    4096, socket.CMSG_SPACE(_UCRED.size))
            except (BlockingIOError, InterruptedError):
                return
            pid = None
            for level, kind, cdata in ancdata:
                if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS:
                    pid, _uid, _gid = _UCRED.unpack(cdata[:_UCRED.size])
            if pid is None:
                continue
            try:
                self.on_message(pid, parse_fields(data))
            except Exception:
                log.exception("notify handler failed")
