"""workspace-logger 진입점: python3 -m workspace_logger

Manager API를 읽기 전용으로 구독해 기록한다 (design.md 6.9). 명령은 보내지 않는다.

기록 종류(type):
  worker   워커 상태·지표. 상태가 바뀌면 즉시, 그 외에는 워커마다 interval_s에 한 번
  removed  워커 등록 해제
  command  쓰기 명령 이력(누가, 무엇을, 성공 여부)
  system   Manager 클라이언트(REST·logger·Manager API를 쓰는 워커) 접속 상태가 바뀔 때
  logger   로거 자신의 시작·종료, Manager 연결·끊김
"""
import argparse
import json
import logging
import os
import signal
import socket
import time

from .recorder import Recorder

log = logging.getLogger("workspace.logger")

# 이 값들이 바뀌면 주기와 상관없이 바로 기록한다
STATE_KEYS = ("desired_state", "actual_state", "pid", "last_exit_code", "last_signal",
              "last_error", "restart_count", "config_version", "applied_version")
RETRY_S = 2.0


class Stop(Exception):
    pass


def _raise_stop(_sig, _frame):
    raise Stop()


class Logger:
    def __init__(self, recorder, interval_s):
        self.rec = recorder
        self.interval = interval_s
        self.last_state = {}    # 워커 id → STATE_KEYS 값
        self.last_written = {}  # 워커 id → monotonic
        self.last_system = None
        self.connected = False

    def disconnected(self, reason):
        """끊김은 한 번만 기록. 다시 붙으면 모든 워커 상태를 새로 기록한다."""
        if self.connected:
            self.rec.write({"type": "logger", "event": "disconnected", "reason": reason})
        self.connected = False
        self.last_state.clear()
        self.last_written.clear()
        self.last_system = None

    def handle(self, msg):
        event, data = msg.get("event"), msg.get("data")
        if event == "worker_status":
            wid = data.get("id")
            state = tuple(data.get(k) for k in STATE_KEYS)
            now = time.monotonic()
            changed = self.last_state.get(wid) != state
            if changed or now - self.last_written.get(wid, -1e9) >= self.interval:
                record = dict(data)
                record["worker_type"] = record.pop("type", None)  # 기록의 type과 겹치지 않게
                self.rec.write({"type": "worker", "changed": changed, **record})
                self.last_state[wid] = state
                self.last_written[wid] = now
        elif event == "worker_removed":
            wid = data.get("id")
            self.last_state.pop(wid, None)
            self.last_written.pop(wid, None)
            self.rec.write({"type": "removed", "id": wid})
        elif event == "command":
            self.rec.write({"type": "command", **data})
        elif event == "system":
            if data != self.last_system:
                self.last_system = data
                self.rec.write({"type": "system", **data})

    def session(self, path, read_timeout):
        """Manager에 붙어 구독이 끊길 때까지 기록. 정상 종료면 Stop을 올린다."""
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.settimeout(read_timeout)
            sock.connect(path)
            sock.sendall(json.dumps({"op": "subscribe",
                                     "topics": ["workers", "system", "commands"],
                                     "period_ms": int(self.interval * 1000)}).encode() + b"\n")
            stream = sock.makefile("rb")
            first = json.loads(stream.readline() or b"{}")
            if not first.get("ok"):
                raise OSError("subscribe refused: %s" % first.get("error"))
            self.rec.write({"type": "logger", "event": "connected"})
            self.connected = True
            # Manager가 변화 없어도 interval마다 상태를 보내므로 read_timeout 동안 조용하면 끊긴 것
            for line in stream:
                try:
                    self.handle(json.loads(line))
                except (ValueError, AttributeError, TypeError) as e:
                    log.warning("bad message: %s", e)
            raise OSError("manager closed the subscription")
        finally:
            sock.close()


def main():
    env = os.environ.get
    ap = argparse.ArgumentParser(prog="workspace-logger")
    data = env("WORKSPACE_DATA", "")
    ap.add_argument("--socket", default=env(
        "WORKSPACE_MANAGER_SOCKET", os.path.join(data, "run", "manager-logger.sock")))
    ap.add_argument("--dir", default=env("WORKSPACE_LOGGER_DIR", os.path.join(data, "records")))
    ap.add_argument("--interval", type=float, default=float(env("WORKSPACE_LOGGER_INTERVAL_S", "1")))
    ap.add_argument("--max-file-mb", type=float, default=float(env("WORKSPACE_LOGGER_MAX_FILE_MB", "64")))
    ap.add_argument("--max-total-mb", type=float,
                    default=float(env("WORKSPACE_LOGGER_MAX_TOTAL_MB", "2048")))
    ap.add_argument("--log-level", default=env("WORKSPACE_LOG_LEVEL", "INFO"))
    args = ap.parse_args()
    logging.basicConfig(level=args.log_level,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    rec = Recorder(args.dir, int(args.max_file_mb * 2 ** 20), int(args.max_total_mb * 2 ** 20))
    logger = Logger(rec, args.interval)
    signal.signal(signal.SIGTERM, _raise_stop)
    signal.signal(signal.SIGINT, _raise_stop)
    rec.write({"type": "logger", "event": "start", "pid": os.getpid()})
    try:
        while True:
            try:
                logger.session(args.socket, read_timeout=max(5.0, args.interval * 5))
            except (OSError, ValueError) as e:
                # Manager가 다시 뜰 때까지 조용히 재시도
                if logger.connected:
                    log.warning("manager unavailable: %s", e)
                logger.disconnected(str(e))
            time.sleep(RETRY_S)
    except Stop:
        rec.write({"type": "logger", "event": "stop"})
    finally:
        rec.close()


if __name__ == "__main__":
    main()
