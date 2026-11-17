"""텔레메트리 스트림 중계 (design.md 9.7): 워커 스트림 소켓 → 브라우저 SSE.

    워커 PUB ──(Unix 소켓, JSON 한 줄씩)──▶ Upstream(워커당 하나) ──▶ Subscriber(브라우저마다) ──SSE──▶ 화면

- 워커마다 위쪽 연결은 하나. 화면이 몇 개든 워커 쪽 부담은 같다
- Upstream은 최신 상태(주제별 값, 최근 이벤트)를 들고 있어, 새 화면에는 바로 기준 상태(kf)를 준다
- 화면마다 hz(초당 최대 전송 수)를 정한다. 그 사이에 온 변경은 주제별로 합쳐 최신 값만 보낸다(이벤트는 모두)
- 화면이 느리면 그 화면만 끊긴다(쓰기 시간 제한). 워커와 다른 화면은 영향이 없다
- 워커가 다시 시작하면 다시 붙고, 화면에는 새 기준 상태를 준다. 끊긴 동안은 {"stream":"down"}을 한 번 보낸다

워커 메시지:  {"seq","kf","t","topics":{...},"events":[...],"events_next"}
화면 메시지:  {"seq","kf","t","topics":{...},"events":[...],"events_next","stream":"up"|"down"}
"""
import json
import logging
import socket
import threading
import time

log = logging.getLogger("workspace.rest.stream")

MAX_EVENTS = 200          # 기준 상태에 담는 최근 이벤트 수
RETRY_SEC = 1.0           # 워커 스트림 소켓에 다시 붙는 간격
IDLE_CLOSE_SEC = 5.0      # 화면이 모두 떠난 뒤 위쪽 연결을 닫기까지
PING_SEC = 15.0           # 보낼 것이 없을 때 연결 유지용 주석(프록시·브라우저 시간 제한 방지)
MAX_LINE = 4 * 1024 * 1024


class Subscriber:
    """화면 하나. Upstream 스레드가 offer()로 넣고, HTTP 스레드가 take()로 꺼낸다."""

    def __init__(self, hz):
        self.min_interval = 1.0 / hz
        self._cv = threading.Condition()
        self._kf = False
        self._topics = {}
        self._events = []
        self._extra = {}
        self._dirty = False
        self._seq = 0
        self.closed = False

    def offer(self, msg, keyframe=False):
        with self._cv:
            if keyframe:
                # 기준 상태는 쌓인 것을 대신한다
                self._kf = True
                self._topics = dict(msg.get("topics") or {})
                self._events = list(msg.get("events") or [])
            else:
                self._topics.update(msg.get("topics") or {})
                self._events.extend(msg.get("events") or [])
                if len(self._events) > MAX_EVENTS:
                    del self._events[:-MAX_EVENTS]
            for k in ("t", "events_next", "stream"):
                if k in msg:
                    self._extra[k] = msg[k]
            self._dirty = True
            self._cv.notify()

    def take(self, timeout):
        """보낼 메시지(합친 것) 또는 None(시간 안에 없음)"""
        with self._cv:
            if not self._dirty and not self.closed:
                self._cv.wait(timeout)
            if not self._dirty:
                return None
            self._seq += 1
            out = {"seq": self._seq, "kf": self._kf, "topics": self._topics}
            if self._events:
                out["events"] = self._events
            out.update(self._extra)
            self._kf, self._topics, self._events, self._extra, self._dirty = False, {}, [], {}, False
            return out

    def close(self):
        with self._cv:
            self.closed = True
            self._cv.notify()


class Upstream:
    """워커 하나의 스트림 소켓 연결(스레드 하나)."""

    def __init__(self, worker_id, open_source, on_idle):
        self.worker_id = worker_id
        self._open = open_source          # () -> 소스(연결된 것)
        self._on_idle = on_idle
        self._lock = threading.Lock()
        self._subs = set()
        self._topics = {}
        self._events = []
        self._last_event = 0
        self._extra = {}
        self._up = False
        self._idle_since = None
        self._stop = False
        self._dead = False                # 정리하기로 정함(더 받지 않는다)
        self._thread = threading.Thread(target=self._run, name="stream-" + worker_id, daemon=True)
        self._thread.start()

    def subscribe(self, sub):
        """정리 중인 Upstream이면 False(Hub가 새로 만든다)"""
        with self._lock:
            if self._dead:
                return False
            self._subs.add(sub)
            self._idle_since = None
            snapshot = {"topics": dict(self._topics), "events": list(self._events),
                        "stream": "up" if self._up else "down"}
            snapshot.update(self._extra)
        sub.offer(snapshot, keyframe=True)
        return True

    def unsubscribe(self, sub):
        with self._lock:
            self._subs.discard(sub)
            if not self._subs:
                self._idle_since = time.monotonic()

    def _idle(self):
        with self._lock:
            return self._idle_locked()

    def _idle_locked(self):
        return (not self._subs and self._idle_since is not None
                and time.monotonic() - self._idle_since > IDLE_CLOSE_SEC)

    def _retire(self):
        """화면이 모두 떠난 지 오래면 정리하기로 정한다(subscribe와 같은 잠금 안에서)"""
        with self._lock:
            if self._idle_locked():
                self._dead = True
            return self._dead

    def _broadcast(self, msg, keyframe=False):
        with self._lock:
            subs = list(self._subs)
        for s in subs:
            s.offer(msg, keyframe)

    def _set_up(self, up):
        if self._up == up:
            return
        self._up = up
        if not up:
            self._broadcast({"stream": "down"})

    def _apply(self, msg, fresh):
        """워커 메시지 → 상태 갱신, 화면에 보낼 것"""
        events = msg.get("events") or []
        with self._lock:
            if fresh:
                # 다시 붙은 뒤 첫 기준 상태: 워커가 새로 시작했을 수 있다(이벤트 번호도 처음부터)
                self._topics = {}
                self._events = []
                self._last_event = 0
            self._topics.update(msg.get("topics") or {})
            new = [e for e in events if e.get("seq", 0) > self._last_event]
            if new:
                self._last_event = new[-1].get("seq", self._last_event)
                self._events.extend(new)
                del self._events[:-MAX_EVENTS]
            for k in ("t", "events_next"):
                if k in msg:
                    self._extra[k] = msg[k]
            if fresh:
                out = {"topics": dict(self._topics), "events": list(self._events), "stream": "up"}
                out.update(self._extra)
            else:
                out = {"topics": msg.get("topics") or {}, "events": new}
                for k in ("t", "events_next"):
                    if k in msg:
                        out[k] = msg[k]
        self._broadcast(out, keyframe=fresh)

    def _run(self):
        while not self._stop:
            if self._retire():
                break
            try:
                src = self._open()
            except (OSError, LookupError) as e:
                log.debug("stream %s: %s", self.worker_id, e)
                self._set_up(False)
                time.sleep(RETRY_SEC)
                continue
            log.info("stream %s: connected %s", self.worker_id, src.name)
            fresh = True
            try:
                for msg in src.messages(lambda: self._stop or self._idle()):
                    # 붙은 직후에는 기준 상태부터 받는다. 그 전 것은 버린다
                    if fresh and not msg.get("kf"):
                        continue
                    self._apply(msg, fresh)
                    if fresh:
                        self._up = True
                    fresh = False
            except (OSError, ValueError) as e:
                log.info("stream %s: %s", self.worker_id, e)
            finally:
                src.close()
            self._set_up(False)
            if not self._stop:
                time.sleep(RETRY_SEC)
        with self._lock:
            self._dead = True
        self._on_idle(self)

    def stop(self):
        self._stop = True


# ---- 위쪽 소스 -----------------------------------------------------------------------------
# messages(should_stop)는 메시지(dict)를 차례로 내고, 연결이 끝나면 멈춘다. 연결 실패는 OSError.

class UnixLineSource:
    """워커 스트림 소켓(JSON 한 줄씩). 보드 REST가 쓴다."""

    def __init__(self, path):
        self.name = path
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self.sock.settimeout(1.0)
            self.sock.connect(path)
        except OSError:
            self.sock.close()
            raise

    def messages(self, should_stop):
        buf = b""
        while not should_stop():
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                return
            buf += chunk
            if len(buf) > MAX_LINE:
                raise ValueError("line too long")
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    yield json.loads(line)
                except ValueError:
                    continue

    def close(self):
        self.sock.close()


class Hub:
    """워커 id → Upstream. 화면이 붙을 때 만들고, 모두 떠나면 Upstream이 스스로 정리한다."""

    def __init__(self, open_source):
        self._open = open_source          # (worker_id) -> 소스 (UnixLineSource). 실패는 OSError·LookupError
        self._lock = threading.Lock()
        self._ups = {}

    def subscribe(self, worker_id, hz):
        sub = Subscriber(hz)
        with self._lock:
            up = self._ups.get(worker_id)
            if up is None or not up.subscribe(sub):
                up = Upstream(worker_id, lambda: self._open(worker_id), self._gone)
                self._ups[worker_id] = up
                up.subscribe(sub)
        return up, sub

    def _gone(self, up):
        with self._lock:
            if self._ups.get(up.worker_id) is up:
                del self._ups[up.worker_id]

    def stats(self):
        with self._lock:
            return {k: len(v._subs) for k, v in self._ups.items()}


def serve_sse(wfile, up, sub, should_stop=lambda: False):
    """SSE 응답 본문을 쓴다(헤더는 호출한 쪽). 화면이 끊으면 돌아온다."""
    last = 0.0
    last_write = time.monotonic()
    try:
        while not should_stop():
            wait = max(0.0, last + sub.min_interval - time.monotonic())
            if wait > 0:
                time.sleep(wait)
            msg = sub.take(timeout=1.0)
            now = time.monotonic()
            if msg is None:
                if now - last_write >= PING_SEC:
                    wfile.write(b": ping\n\n")
                    wfile.flush()
                    last_write = now
                continue
            data = json.dumps(msg, ensure_ascii=False, separators=(",", ":"))
            wfile.write(b"data: " + data.encode("utf-8") + b"\n\n")
            wfile.flush()
            last = last_write = now
    except (OSError, ValueError):
        pass
    finally:
        up.unsubscribe(sub)
        sub.close()
