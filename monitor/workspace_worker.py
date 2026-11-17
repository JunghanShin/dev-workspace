"""Python 워커 도우미: 워크스페이스 워커 계약(design.md 9장)의 공통 부분. 표준 라이브러리만.

C++ 워커는 libworker가 계약을 맡는다. Python 워커는 이 파일을 워커 폴더에 복사해 쓴다(사본 방식, 원본은
workers/_template/python/workspace_worker.py. 사본이 원본과 같은지 dev/test_workers.py가 확인).

    import workspace_worker as ww

    def main():
        try:
            wid, cfg = ww.load_config(schema={"interval_ms": (int, 1000)})
        except ww.ConfigError as e:
            print("config error: %s" % e, file=sys.stderr); return ww.EXIT_CONFIG
        wd = ww.Watchdog(3.0)          # 처리 루프에서 wd.kick()
        hb = ww.Heartbeat()            # 초기화가 끝나면 hb.ready(), 루프에서 hb.watchdog(progress, {"x_fps": 30})
        stop = ww.StopFlag()           # SIGTERM·SIGINT → stop.requested
        ...

계약: 실행 인자 `--config <파일>`({"id", "config_version", "config"}), READY 한 번, 처리 루프에서 1초마다
WATCHDOG + 지표, 자체 워치독(SIGALRM 기본 동작), SIGTERM이면 정리 후 exit 0, 설정 오류 exit 1, 장치 오류 exit 2.
"""
import argparse
import json
import os
import signal
import socket

VERSION = "py-1.1"  # READY의 libworker 칸으로 알린다(화면 워커 목록에 보임)

EXIT_OK = 0
EXIT_CONFIG = 1
EXIT_DEVICE = 2
EXIT_SOCKET = 3
EXIT_FAILED = 4


class ConfigError(Exception):
    pass


def load_config(schema=None, argv=None, description="workspace worker"):
    """--config 파일 → (워커 id, config dict). schema {키: (형, 기본값)}를 주면 모르는 키는 오류, 형 변환·기본값.
    argparse의 기본 exit 2는 장치 오류로 오판되므로 ConfigError로 바꾼다"""
    ap = argparse.ArgumentParser(description=description)
    ap.add_argument("--config", required=True, help="worker config file written by the Manager")

    def arg_error(message):
        raise ConfigError(message)

    ap.error = arg_error
    args = ap.parse_args(argv)
    try:
        with open(args.config, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        raise ConfigError("cannot read %s: %s" % (args.config, e))
    raw = data.get("config", {}) if isinstance(data, dict) else None
    if not isinstance(raw, dict):
        raise ConfigError("config must be an object")
    wid = data.get("id") or os.environ.get("WORKSPACE_WORKER_ID") \
        or os.path.splitext(os.path.basename(args.config))[0]
    if schema is None:
        return wid, raw
    unknown = set(raw) - set(schema)
    if unknown:
        raise ConfigError("unknown config keys: %s" % ", ".join(sorted(unknown)))
    cfg = {}
    for key, (kind, default) in schema.items():
        try:
            cfg[key] = kind(raw.get(key, default))
        except (TypeError, ValueError):
            raise ConfigError("%s must be %s" % (key, kind.__name__))
    return wid, cfg


class Watchdog:
    """자체 워치독(design.md 6.6). 처리 루프에서 kick()으로 커널 타이머를 다시 맞춘다.
    SIGALRM은 핸들러 없이 기본 동작(종료)이라 루프·GIL이 묶여도 커널이 끝낸다."""

    def __init__(self, sec):
        self.sec = sec
        signal.signal(signal.SIGALRM, signal.SIG_DFL)

    def kick(self, sec=None):
        signal.setitimer(signal.ITIMER_REAL, sec if sec is not None else self.sec)

    def stop(self):
        signal.setitimer(signal.ITIMER_REAL, 0)


class Heartbeat:
    """Manager로 READY / WATCHDOG 송신(design.md 9.5). non-blocking, 실패는 무시한다.
    WORKSPACE_NOTIFY_SOCKET이 없으면(워크스페이스 밖 단독 실행) 보내지 않는다"""

    def __init__(self):
        self.path = os.environ.get("WORKSPACE_NOTIFY_SOCKET")
        self.worker_id = os.environ.get("WORKSPACE_WORKER_ID", "")
        self.sock = None
        if self.path:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            self.sock.setblocking(False)

    def send(self, text):
        if self.sock is None:
            return
        try:
            self.sock.sendto(("%s id=%s" % (text, self.worker_id)).encode(), self.path)
        except OSError:
            pass

    def ready(self):
        self.send("READY=1 libworker=%s" % VERSION)

    def watchdog(self, progress=0, metrics=None, config_version=None):
        """metrics: {"<모듈>_<이름>": 숫자}. 처리 루프 안에서 부른다(별도 스레드에서 보내지 않는다)"""
        parts = ["WATCHDOG=1", "progress=%d" % progress]
        if config_version is not None:
            parts.append("config_version=%d" % config_version)
        parts += ["%s=%s" % kv for kv in (metrics or {}).items()]
        self.send(" ".join(parts))


class StopFlag:
    """SIGTERM·SIGINT → requested = True. on_stop(선택)은 막힌 읽기를 깨울 때(자식 프로세스 terminate 등)"""

    def __init__(self, on_stop=None):
        self.requested = False
        self._on_stop = on_stop
        signal.signal(signal.SIGTERM, self._handle)
        signal.signal(signal.SIGINT, self._handle)

    def _handle(self, _sig, _frame):
        self.requested = True
        if self._on_stop is not None:
            self._on_stop()



# ---- 워커 간 통신(design.md 6.6.7, 9.6, 9.7, 6.7). C++ libworker·libframebus와 같은 이름·같은 통로 ----------------
#
#   받기(이 워커가 제공):  CommandServer(명령 소켓), Publisher(스트림 발행)
#   부르기(다른 워커로):   CommandClient(요청·응답, 별도 스레드), Subscriber(구독, 다시 붙음), FramebusConsumer(데이터 버스)
#
# 형식은 언어와 무관하다: 명령·스트림은 Unix 소켓 JSON 한 줄, 데이터 버스는 공유 메모리 + SOCK_SEQPACKET 알림.
# 그래서 C++ 워커와 Python 워커가 서로 그대로 주고받는다. 데이터 버스 생산자는 아직 없다(C++ libframebus만).

import errno
import mmap
import queue
import select
import struct
import threading
import time


def _runtime_dir_socket(name):
    d = os.environ.get("XDG_RUNTIME_DIR")
    return os.path.join(d if d else "/tmp", name)


def default_command_socket(worker_id):
    """다른 워커(또는 이 워커)의 명령 소켓 기본 위치(libworker default_command_socket과 같음)"""
    data = os.environ.get("WORKSPACE_DATA")
    return os.path.join(data, "run", "cmd", worker_id + ".sock") if data else _runtime_dir_socket(worker_id + ".sock")


def default_stream_socket(worker_id):
    data = os.environ.get("WORKSPACE_DATA")
    return (os.path.join(data, "run", "stream", worker_id + ".sock") if data
            else _runtime_dir_socket(worker_id + ".stream.sock"))


def default_socket(worker_id, kind):
    """kind 예: "frames"(데이터 버스 알림 소켓)"""
    data = os.environ.get("WORKSPACE_DATA")
    return (os.path.join(data, "run", kind, worker_id + ".sock") if data
            else _runtime_dir_socket("%s.%s.sock" % (worker_id, kind)))


def ok_reply(data=None):
    return {"ok": True, "data": data if data is not None else {}}


def error_reply(code, message):
    return {"ok": False, "error": {"code": code, "message": message}}


def send_request(path, request, timeout=2.0):
    """JSON 한 줄을 보내고 응답 한 줄을 받는다(요청 1건 = 연결 1개). 실패하면 {"ok": False, "error": {...}}"""
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(timeout)
            s.connect(path)
            s.sendall((json.dumps(request, ensure_ascii=False) + "\n").encode())
            buf = b""
            end = time.monotonic() + timeout
            while not buf.endswith(b"\n"):
                s.settimeout(max(0.01, end - time.monotonic()))
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
        return json.loads(buf.decode())
    except socket.timeout:
        return error_reply("timeout", "no reply from %s" % path)
    except (OSError, ValueError) as e:
        return error_reply("unavailable", "%s: %s" % (path, e))


def send_command(path, cmd, args=None, timeout=2.0):
    return send_request(path, {"cmd": cmd, "args": args or {}}, timeout)


def _listen(path):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(path)
    os.chmod(path, 0o600)
    s.listen(16)
    s.setblocking(False)
    return s


class CommandServer:
    """명령 소켓 서버(9.6). handler(cmd, args) → 응답 dict(ok_reply / error_reply). 처리 루프에서 serve(timeout)를 부른다.
    get_status 같은 명령도 handler가 답한다. 느린 클라이언트가 루프를 막지 않게 논블로킹, 연결마다 1초 상한"""

    def __init__(self, path, handler, max_bytes=65536):
        self.path, self.handler, self.max_bytes = path, handler, max_bytes
        self.sock = None
        self._conns = {}  # sock → [버퍼, 마감]

    def open(self):
        self.sock = _listen(self.path)

    def close(self):
        for c in list(self._conns):
            c.close()
        self._conns.clear()
        if self.sock:
            self.sock.close()
            self.sock = None
        try:
            os.unlink(self.path)
        except OSError:
            pass

    def fileno(self):
        return self.sock.fileno()

    def serve(self, timeout=0.0):
        """한 번 돌린다: 접속 받기, 읽기, 끝난 줄 처리. timeout(초) 동안 기다릴 수 있다"""
        rl = [self.sock] + list(self._conns)
        r, _, _ = select.select(rl, [], [], timeout)
        now = time.monotonic()
        for s in r:
            if s is self.sock:
                try:
                    c, _ = self.sock.accept()
                    c.setblocking(False)
                    self._conns[c] = [b"", now + 1.0]
                except OSError:
                    pass
                continue
            st = self._conns.get(s)
            try:
                chunk = s.recv(65536)
            except OSError:
                chunk = b""
            if not chunk:
                self._drop(s)
                continue
            st[0] += chunk
            if b"\n" in st[0] or len(st[0]) > self.max_bytes:
                self._answer(s, st[0].split(b"\n", 1)[0])
        for s, st in list(self._conns.items()):
            if now > st[1]:
                self._drop(s)

    def _answer(self, s, line):
        try:
            req = json.loads(line.decode())
            if not isinstance(req, dict) or not isinstance(req.get("cmd"), str):
                raise ValueError("cmd is required")
            args = req.get("args") or {}
            resp = self.handler(req["cmd"], args) if isinstance(args, dict) else error_reply("bad_request", "args must be an object")
        except ValueError as e:
            resp = error_reply("bad_request", "invalid request: %s" % e)
        except Exception as e:  # handler 오류가 워커를 죽이지 않게
            resp = error_reply("internal", str(e))
        if resp is None:
            resp = error_reply("bad_request", "unknown command")
        try:
            s.setblocking(True)
            s.settimeout(1.0)
            s.sendall((json.dumps(resp, ensure_ascii=False) + "\n").encode())
        except OSError:
            pass
        self._drop(s)

    def _drop(self, s):
        self._conns.pop(s, None)
        try:
            s.close()
        except OSError:
            pass


class Publisher:
    """스트림 발행(9.7). publish(dict)는 막히지 않는다. 구독자마다 보낼 버퍼가 있고 max_backlog를 넘으면 그 구독자를 끊는다.
    새 구독자가 붙으면 keyframe_requested()가 참 → 전체 상태(kf: true)를 한 번 발행한다. 처리 루프에서 pump()를 부른다.
    한 줄 형식(C++와 같음): {"seq", "kf", "t", "topics": {주제: {...}}, "events": [...], "events_next"}"""

    def __init__(self, path, max_backlog=512 * 1024, max_subscribers=16):
        self.path, self.max_backlog, self.max_subscribers = path, max_backlog, max_subscribers
        self.sock = None
        self._subs = {}  # sock → 보낼 바이트
        self._kf = False
        self.dropped_subscribers = 0

    def open(self):
        self.sock = _listen(self.path)

    def close(self):
        for s in list(self._subs):
            s.close()
        self._subs.clear()
        if self.sock:
            self.sock.close()
            self.sock = None
        try:
            os.unlink(self.path)
        except OSError:
            pass

    def subscribers(self):
        return len(self._subs)

    def keyframe_requested(self):
        k, self._kf = self._kf, False
        return k

    def pump(self):
        """접속 받기·밀린 바이트 보내기·끊긴 구독자 정리(논블로킹)"""
        while True:
            try:
                c, _ = self.sock.accept()
            except (BlockingIOError, OSError):
                break
            if len(self._subs) >= self.max_subscribers:
                c.close()
                continue
            c.setblocking(False)
            self._subs[c] = b""
            self._kf = True
        for s in list(self._subs):
            self._flush(s)

    def publish(self, obj):
        line = (json.dumps(obj, ensure_ascii=False) + "\n").encode()
        for s in list(self._subs):
            self._subs[s] += line
            if len(self._subs[s]) > self.max_backlog:
                self.dropped_subscribers += 1
                self._drop(s)
            else:
                self._flush(s)

    def _flush(self, s):
        buf = self._subs.get(s)
        if not buf:
            return
        try:
            n = s.send(buf)
            self._subs[s] = buf[n:]
        except BlockingIOError:
            pass
        except OSError:
            self._drop(s)

    def _drop(self, s):
        self._subs.pop(s, None)
        try:
            s.close()
        except OSError:
            pass


class Subscriber:
    """다른 워커의 스트림 구독(peer.hpp Subscriber). 별도 스레드가 붙고, 끊기면 retry_sec마다 다시 붙는다.
    next() → {"down": bool, "kf", "topics", "events"} 또는 None. 끊기면 {"down": True}가 한 번 온다"""

    def __init__(self, peer_id, socket_path=None, retry_sec=1.0, queue_size=256):
        self.peer = peer_id
        self.path = socket_path or default_stream_socket(peer_id)
        self.retry_sec = retry_sec
        self._q = queue.Queue(maxsize=queue_size)
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, name="sub", daemon=True)
        self.connected = False
        self.received = 0
        self.reconnects = 0

    def start(self):
        self._t.start()

    def stop(self):
        self._stop.set()
        self._t.join(timeout=3)

    def next(self):
        try:
            return self._q.get_nowait()
        except queue.Empty:
            return None

    def _put(self, u):
        if self._q.full():  # 최신이 중요: 오래된 것부터 버림
            try:
                self._q.get_nowait()
            except queue.Empty:
                pass
        self._q.put_nowait(u)

    def _run(self):
        while not self._stop.is_set():
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                    s.settimeout(1.0)
                    s.connect(self.path)
                    if self.received or self.reconnects:
                        self.reconnects += 1
                    self.connected = True
                    buf = b""
                    while not self._stop.is_set():
                        try:
                            chunk = s.recv(65536)
                        except socket.timeout:
                            continue
                        if not chunk:
                            break
                        buf += chunk
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            try:
                                j = json.loads(line.decode())
                            except ValueError:
                                continue
                            self.received += 1
                            self._put({"down": False, "kf": bool(j.get("kf")), "topics": j.get("topics") or {},
                                       "events": j.get("events") or []})
            except OSError:
                pass
            if self.connected:
                self.connected = False
                self._put({"down": True, "kf": False, "topics": {}, "events": []})
            self._stop.wait(self.retry_sec)


class CommandClient:
    """다른 워커에 비동기 명령(peer.hpp CommandClient). send()는 번호(1부터, 큐가 차면 0)를 돌려주고 별도 스레드가 보낸다.
    next() → (번호, cmd, 응답 dict) 또는 None. 처리 루프는 기다리지 않는다"""

    def __init__(self, peer_id, socket_path=None, timeout=1.5, queue_size=64):
        self.peer = peer_id
        self.path = socket_path or default_command_socket(peer_id)
        self.timeout = timeout
        self._req = queue.Queue(maxsize=queue_size)
        self._rep = queue.Queue()
        self._next_id = 1
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, name="cmd", daemon=True)
        self.reachable = False

    def start(self):
        self._t.start()

    def stop(self):
        self._stop.set()
        self._t.join(timeout=3)

    def send(self, cmd, args=None):
        with self._lock:
            rid = self._next_id
            self._next_id += 1
        try:
            self._req.put_nowait((rid, cmd, args or {}))
        except queue.Full:
            return 0
        return rid

    def next(self):
        try:
            return self._rep.get_nowait()
        except queue.Empty:
            return None

    def _run(self):
        while not self._stop.is_set():
            try:
                rid, cmd, args = self._req.get(timeout=0.2)
            except queue.Empty:
                continue
            resp = send_command(self.path, cmd, args, self.timeout)
            err = (resp.get("error") or {}).get("code") if not resp.get("ok") else None
            self.reachable = err not in ("unavailable", "timeout")
            self._rep.put((rid, cmd, resp))


class FramebusConsumer:
    """데이터 버스 소비자(libframebus Consumer와 같은 통로). connect() 뒤 처리 루프에서 next(timeout_ms):
    Frame(slot, data(읽기 전용 memoryview), bytes, frame_id, ts_mono_ns) / None(시간 초과) / FramebusConsumer.DISCONNECTED.
    쓴 프레임은 반드시 release(frame). mode: "latest"(최신만, 밀린 것은 반납) / "inorder"(받은 순서대로 전부)"""

    DISCONNECTED = "disconnected"
    _MAGIC, _VERSION, _FRAME, _RELEASE = 0x53554246, 1, 1, 2
    _HELLO = struct.Struct("<7I16s")    # magic version slots slot_bytes width height stride pixel[16]
    _MSG = struct.Struct("<IiQqII")      # type slot frame_id ts_mono_ns bytes pad

    class Frame:
        __slots__ = ("slot", "data", "bytes", "frame_id", "ts_mono_ns")

        def __init__(self, slot, data, nbytes, frame_id, ts):
            self.slot, self.data, self.bytes, self.frame_id, self.ts_mono_ns = slot, data, nbytes, frame_id, ts

    def __init__(self, path, mode="latest"):
        if mode not in ("latest", "inorder"):
            raise ValueError("mode must be latest or inorder")
        self.path, self.mode = path, mode
        self.sock = None
        self.map = None
        self.slots = self.slot_bytes = 0
        self.format = {}
        self.skipped = 0
        self._pending = []

    def connected(self):
        return self.sock is not None

    def connect(self):
        self.close()
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            s.settimeout(2.0)
            s.connect(self.path)
            data, anc, _, _ = s.recvmsg(self._HELLO.size, socket.CMSG_SPACE(struct.calcsize("i")))
            fds = []
            for level, typ, cdata in anc:
                if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                    fds += list(struct.unpack("%di" % (len(cdata) // 4), cdata[:len(cdata) - len(cdata) % 4]))
            if len(data) != self._HELLO.size or not fds:
                for fd in fds:
                    os.close(fd)
                s.close()
                return False
            magic, ver, slots, slot_bytes, w, h, stride, pixel = self._HELLO.unpack(data)
            if magic != self._MAGIC or ver != self._VERSION or not slots or not slot_bytes:
                for fd in fds:
                    os.close(fd)
                s.close()
                return False
            self.map = mmap.mmap(fds[0], slots * slot_bytes, mmap.MAP_SHARED, mmap.PROT_READ)
            for fd in fds:
                os.close(fd)
            self.slots, self.slot_bytes = slots, slot_bytes
            self.format = {"width": w, "height": h, "stride": stride, "pixel": pixel.split(b"\0", 1)[0].decode()}
            s.setblocking(False)
            self.sock = s
            return True
        except OSError:
            self.close()
            return False

    def close(self):
        self._pending = []
        if self.map is not None:
            try:
                self.map.close()
            except BufferError:  # 아직 쥐고 있는 memoryview가 있으면 GC가 정리
                pass
            self.map = None
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def release(self, frame):
        self._send_release(frame.slot)

    def _send_release(self, slot):
        if self.sock is None:
            return
        try:
            self.sock.send(self._MSG.pack(self._RELEASE, slot, 0, 0, 0, 0), socket.MSG_DONTWAIT)
        except OSError:
            pass

    def _fill(self):
        if not self._pending:
            return None
        slot, fid, ts, nb = self._pending.pop(0)
        off = slot * self.slot_bytes
        nb = min(nb, self.slot_bytes)
        return self.Frame(slot, memoryview(self.map)[off:off + nb], nb, fid, ts)

    def next(self, timeout_ms=100):
        if self.sock is None:
            return self.DISCONNECTED
        if self.mode == "inorder" and self._pending:
            return self._fill()
        r, _, x = select.select([self.sock], [], [self.sock], timeout_ms / 1000.0)
        if not r and not x:
            return None
        while True:
            try:
                m = self.sock.recv(self._MSG.size)
            except BlockingIOError:
                break
            except OSError:
                self.close()
                return self.DISCONNECTED
            if not m:
                self.close()
                return self.DISCONNECTED
            if len(m) != self._MSG.size:
                continue
            typ, slot, fid, ts, nb, _ = self._MSG.unpack(m)
            if typ != self._FRAME or slot < 0 or slot >= self.slots:
                continue
            if self.mode == "latest" and self._pending:
                self._send_release(self._pending[0][0])
                self._pending = []
                self.skipped += 1
            self._pending.append((slot, fid, ts, nb))
        f = self._fill()
        return f
