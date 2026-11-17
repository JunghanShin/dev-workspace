"""기록 파일: 부팅 단위 JSON Lines, 파일·전체 용량 제한.

파일 이름: {처음 쓴 시각}_{boot_id 앞 8자}.jsonl
  - 같은 부팅에서 로거가 다시 뜨면 그 부팅의 마지막 파일에 이어 쓴다
  - 파일이 max_file_bytes를 넘으면 새 파일로 넘어간다(이름순 = 시간순)
  - 전체가 max_total_bytes를 넘으면 오래된 파일부터 지운다(쓰는 중인 파일 제외)
"""
import json
import logging
import os
import time
from datetime import datetime

log = logging.getLogger("workspace.logger")

SUFFIX = ".jsonl"
FSYNC_INTERVAL_S = 5.0


def boot_id():
    try:
        with open("/proc/sys/kernel/random/boot_id", encoding="ascii") as f:
            return f.read().strip().replace("-", "")[:8]
    except OSError:
        return "noboot00"


def now_iso():
    return datetime.now().astimezone().isoformat(timespec="milliseconds")


class Recorder:
    def __init__(self, directory, max_file_bytes, max_total_bytes):
        self.dir = directory
        self.max_file = max_file_bytes
        self.max_total = max_total_bytes
        self.boot = boot_id()
        os.makedirs(directory, exist_ok=True)
        self.f = None
        self.path = None
        self.last_sync = 0.0
        existing = sorted(n for n in os.listdir(directory)
                          if n.endswith("_%s%s" % (self.boot, SUFFIX)))
        self._open(os.path.join(directory, existing[-1]) if existing else None)

    def _open(self, path=None):
        if self.f is not None:
            self._sync()
            self.f.close()
        os.makedirs(self.dir, exist_ok=True)  # 데이터 폴더가 지워졌어도 다시 만든다
        if path is None:
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
            path = os.path.join(self.dir, "%s_%s%s" % (stamp, self.boot, SUFFIX))
            if path == self.path:  # 같은 초에 넘어가면 이름이 겹친다
                path = path[:-len(SUFFIX)] + "-%d%s" % (int(time.time() * 1000) % 1000, SUFFIX)
        self.path = path
        self.f = open(path, "a", encoding="utf-8", newline="\n")
        log.info("recording to %s", path)
        self._prune()

    def _prune(self):
        files = []
        try:
            names = os.listdir(self.dir)
        except OSError:
            return
        for name in names:
            if not name.endswith(SUFFIX):
                continue
            p = os.path.join(self.dir, name)
            try:
                files.append((name, p, os.path.getsize(p)))
            except OSError:
                continue
        files.sort()
        total = sum(size for _, _, size in files)
        for _, p, size in files:
            if total <= self.max_total:
                break
            if p == self.path:
                continue
            try:
                os.remove(p)
                total -= size
                log.info("removed old record %s", p)
            except OSError as e:
                log.warning("cannot remove %s: %s", p, e)

    def _sync(self):
        self.f.flush()
        try:
            os.fsync(self.f.fileno())
        except OSError:
            pass
        self.last_sync = time.monotonic()

    def write(self, record):
        """record에 시각 t를 붙여 한 줄로 쓴다."""
        line = json.dumps(dict(t=now_iso(), **record), ensure_ascii=False,
                          separators=(",", ":")) + "\n"
        self.f.write(line)
        self.f.flush()  # 로거가 죽어도 커널까지는 넘긴다. 전원 차단 대비 fsync는 주기적으로
        if time.monotonic() - self.last_sync >= FSYNC_INTERVAL_S:
            self._sync()
        if self.f.tell() >= self.max_file:
            self._open()

    def close(self):
        if self.f is not None:
            self._sync()
            self.f.close()
            self.f = None
