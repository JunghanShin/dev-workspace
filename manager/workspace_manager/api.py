"""Manager API: Unix 소켓 요청-응답·구독 (design.md 6.2).

클라이언트는 REST, logger, Manager API를 쓰는 워커(워커 선언 "manager_api": true, 예: VSM 개발)다.
모든 프로세스가 같은 계정으로 돌므로 클라이언트는 접속한 소켓으로 구분한다:
  {run_dir}/manager-rest.sock, manager-logger.sock   플랫폼 클라이언트. logger는 읽기 전용(기록만)
  {run_dir}/manager-<워커 id>.sock                   Manager API를 쓰는 워커. 자기 워커는 못 건드림
  {run_dir}/manager.sock                             로컬 도구용. 읽기 전용
워크스페이스는 워커 개발 작업대라 운용 모드·제어권을 두지 않는다(design.md 6.1).
"""
import asyncio
import json
import logging
import os
import time

from . import settings as settings_mod
from .supervisor import ApiError

log = logging.getLogger("workspace.manager")

WRITE_OPS = ("set_config", "use_preset", "save_preset", "rename_preset", "delete_preset",
             "set_autostart", "start", "stop")
PLATFORM_CLIENTS = ("rest", "logger")
CONFIG_OPS = ("set_config", "use_preset", "save_preset", "rename_preset", "delete_preset", "set_autostart")
READ_ONLY_CLIENTS = ("logger",)  # 기록만 함. 로컬 도구용 manager.sock(client None)도 읽기 전용


def _line(obj):
    return json.dumps(obj, ensure_ascii=False).encode("utf-8") + b"\n"


class _Subscriber:
    def __init__(self, topics):
        self.topics = topics
        self.pending = {}  # key → 마지막 이벤트 한 건(최신값만 유지)
        self.event = asyncio.Event()

    def offer(self, topic, key, line):
        if topic in self.topics:
            self.pending[key] = line
            self.event.set()


class ApiServer:
    def __init__(self, supervisor, settings, api_workers=()):
        self.sup = supervisor
        self.cfg = settings["api"]
        # 플랫폼 클라이언트(REST, logger) + Manager API를 쓰는 워커(이름 = 워커 id). 이름마다 전용 소켓
        self.api_workers = sorted(api_workers)
        self.client_names = list(PLATFORM_CLIENTS) + self.api_workers
        self.servers = []
        self.subscribers = set()
        self.last_seen = {}
        self.command_seq = 0
        supervisor.listeners.append(self._on_event)

    async def start(self, run_dir):
        os.makedirs(run_dir, exist_ok=True)
        sockets = [(None, os.path.join(run_dir, "manager.sock"))]
        sockets += [(name, os.path.join(run_dir, "manager-%s.sock" % name))
                    for name in self.client_names]
        # 지금 클라이언트가 아닌 manager-*.sock(예전 이름, 빠진 워커)은 지운다: 데이터 폴더를 지금 구성과 맞춘다
        keep = {os.path.basename(p) for _, p in sockets}
        for f in os.listdir(run_dir):
            if f.startswith("manager-") and f.endswith(".sock") and f not in keep:
                try:
                    os.unlink(os.path.join(run_dir, f))
                    log.info("removed stale socket %s", f)
                except OSError as e:
                    log.warning("cannot remove stale socket %s: %s", f, e)
        for client, path in sockets:
            try:
                os.unlink(path)  # 이전 실행이 남긴 소켓 파일
            except FileNotFoundError:
                pass

            async def handle(reader, writer, client=client):
                await self._handle(reader, writer, client)

            server = await asyncio.start_unix_server(
                handle, path=path, limit=self.cfg["max_message_bytes"])
            os.chmod(path, 0o600)  # 같은 계정만
            self.servers.append(server)
        log.info("api listening in %s: manager.sock (read-only), %s",
                 run_dir, ", ".join("manager-%s.sock" % n for n in self.client_names))

    def close(self):
        for server in self.servers:
            server.close()

    # ---- 상태 -----------------------------------------------------------

    def system_status(self):
        now = time.monotonic()
        subscribed = {s.client for s in self.subscribers}
        clients = {}
        for name in self.client_names:
            if name in subscribed:
                clients[name] = "subscribed"
            elif now - self.last_seen.get(name, -1e9) < 10.0:
                clients[name] = "idle"
            else:
                clients[name] = "disconnected"
        return {"clients": clients, "manager_api_workers": self.api_workers,
                "product": settings_mod.PRODUCT, "config_write": settings_mod.CONFIG_WRITE}

    def _on_event(self, event, key, data):
        self._publish("workers", (event, key), {"event": event, "data": data})

    def _publish(self, topic, key, obj):
        line = _line(obj)
        for sub in self.subscribers:
            sub.offer(topic, key, line)

    def _record_command(self, req, client, resp):
        """쓰기 명령 이력 (topic "commands"). 키가 매번 달라 최신값 덮어쓰기에서 빠지지 않는다."""
        self.command_seq += 1
        data = {"op": req["op"], "id": req.get("id"), "client": client, "ok": resp["ok"]}
        if req["op"] == "set_config":
            data["config"] = req.get("config")
            data["save"] = req.get("save", True) is not False
        elif req["op"] == "set_autostart":
            data["autostart"] = req.get("autostart")
        elif req["op"] in ("use_preset", "rename_preset", "delete_preset", "save_preset"):
            data["preset"] = req.get("name") or (req.get("preset") or {}).get("name")
        if not resp["ok"]:
            data["error"] = resp["error"]
        self._publish("commands", ("command", self.command_seq), {"event": "command", "data": data})

    # ---- 연결 처리 ------------------------------------------------------

    async def _handle(self, reader, writer, client):
        if client:
            self.last_seen[client] = time.monotonic()
        try:
            line = await asyncio.wait_for(reader.readline(), self.cfg["read_timeout_s"])
        except (asyncio.TimeoutError, ValueError, ConnectionError):
            # timeout 또는 크기 상한 초과: 느린·비정상 클라이언트가 Manager를 붙잡지 못하게
            writer.close()
            return
        if not line:
            writer.close()
            return
        try:
            req = json.loads(line)
            if not isinstance(req, dict) or not isinstance(req.get("op"), str):
                raise ValueError("op missing")
        except ValueError as e:
            await self._reply(writer, self._error("bad_request", "invalid request: %s" % e))
            return
        if req["op"] == "subscribe":
            await self._subscribe(req, writer, client)
            return
        resp = self._dispatch(req, client)
        if req["op"] in WRITE_OPS:
            self._record_command(req, client, resp)
        await self._reply(writer, resp)

    async def _reply(self, writer, resp):
        try:
            writer.write(_line(resp))
            await asyncio.wait_for(writer.drain(), self.cfg["read_timeout_s"])
        except (asyncio.TimeoutError, ConnectionError):
            pass
        finally:
            writer.close()

    @staticmethod
    def _error(code, message):
        return {"ok": False, "error": {"code": code, "message": message}}

    def _dispatch(self, req, client):
        op = req["op"]
        if op in WRITE_OPS and (client is None or client in READ_ONLY_CLIENTS):
            return self._error("forbidden", "%s is read-only" % (client or "manager.sock"))
        # 제품 모드: 설정 잠금(시작·정지는 됨), 가짜 장치 설정 금지(design.md 11.4)
        if op in CONFIG_OPS and not settings_mod.CONFIG_WRITE:
            return self._error("forbidden", "settings are locked (product)")
        if settings_mod.PRODUCT and op in ("set_config", "save_preset"):
            cfg = req.get("config") if op == "set_config" else (req.get("preset") or {}).get("config")
            if isinstance(cfg, dict) and "sim" in cfg:
                return self._error("bad_request", "sim (fake device) not allowed in product")
        # Manager API를 쓰는 워커는 자기 자신을 정지·설정하지 못한다(스스로 끊기지 않게)
        if op in WRITE_OPS and client in self.api_workers and req.get("id") == client:
            return self._error("forbidden", "worker %s cannot control itself" % client)
        handlers = {
            "ping": lambda: {"pong": True},
            "system": self.system_status,
            "list": self.sup.list,
            "get": lambda: self.sup.get(req.get("id")),
            "get_config": lambda: self.sup.get_config(req.get("id")),
            "get_schema": lambda: self.sup.get_schema(req.get("id")),
            "set_config": lambda: self.sup.set_config(req.get("id"), req.get("config"),
                                                      save=req.get("save", True) is not False),
            "use_preset": lambda: self.sup.use_preset(req.get("id"), req.get("name")),
            "save_preset": lambda: self.sup.save_preset(req.get("id"), req.get("preset")),
            "rename_preset": lambda: self.sup.rename_preset(req.get("id"), req.get("name"), req.get("preset")),
            "delete_preset": lambda: self.sup.delete_preset(req.get("id"), req.get("name")),
            "set_autostart": lambda: self.sup.set_autostart(req.get("id"), req.get("autostart")),
            "start": lambda: self.sup.start(req.get("id")),
            "stop": lambda: self.sup.stop(req.get("id")),
        }
        handler = handlers.get(op)
        if handler is None:
            return self._error("bad_request", "unknown op %r" % op)
        try:
            return {"ok": True, "data": handler()}
        except ApiError as e:
            resp = self._error(e.code, e.message)
            if e.fields:
                resp["error"]["fields"] = e.fields
            return resp
        except Exception as e:
            log.exception("op %s failed", op)
            return self._error("internal", str(e))

    # ---- 구독 -----------------------------------------------------------

    async def _subscribe(self, req, writer, client):
        topics = set(req.get("topics") or ("workers", "system"))
        try:
            period = max(0.1, float(req.get("period_ms", self.cfg["subscribe_period_ms"])) / 1000)
        except (TypeError, ValueError):
            period = self.cfg["subscribe_period_ms"] / 1000
        sub = _Subscriber(topics)
        sub.client = client
        self.subscribers.add(sub)
        timeout = self.cfg["subscriber_timeout_s"]
        next_snapshot = 0.0
        try:
            writer.write(_line({"ok": True, "data": {"subscribed": sorted(topics)}}))
            while True:
                now = time.monotonic()
                if now >= next_snapshot:
                    # 변화가 없어도 주기적으로 전체 상태 push
                    for st in self.sup.list():
                        sub.offer("workers", ("worker_status", st["id"]),
                                  _line({"event": "worker_status", "data": st}))
                    sub.offer("system", ("system", None),
                              _line({"event": "system", "data": self.system_status()}))
                    next_snapshot = now + period
                try:
                    await asyncio.wait_for(sub.event.wait(), max(0.0, next_snapshot - now))
                except asyncio.TimeoutError:
                    pass
                sub.event.clear()
                batch, sub.pending = sub.pending, {}
                for line in batch.values():
                    writer.write(line)
                # 느린 구독자는 끊는다. 기다리는 동안 쌓이는 이벤트는 최신값만 남는다
                await asyncio.wait_for(writer.drain(), timeout)
        except (asyncio.TimeoutError, ConnectionError):
            pass
        finally:
            self.subscribers.discard(sub)
            writer.close()
