"""REST 읽기 전용 기능: 로그, 명령 이력, 프로세스, 전원 모드, 지난 지표 (design.md 6.9).

워크스페이스 UI가 SSH 없이 REST 주소 하나로 보드 상태를 다 보게 한다. 모두 GET이고 보드를 바꾸지 않는다.
경로는 고정이고 워커 id만 받는다(임의 파일 경로·명령 없음).

  GET /logs/workers/{id}?lines=200    워커 로그 끝부분              {"lines": [...]}
  GET /logs/service?lines=200         workspace 서비스 로그(journal)       {"lines": [...]}
  GET /records/commands?lines=200     명령 기록(workspace-logger)         {"lines": [JSON 문자열...]}
  GET /system/processes?lines=12      CPU 순 프로세스               {"processes": [{pid, name, cpu, mem, rss_kb}]}
  GET /system/power_modes             nvpmodel 모드 이름            {"modes": {"0": "MAXN_SUPER", ...}}
  GET /workers/{id}/tasks?pid=N       워커(프로세스 그룹 N = 워커 pid)의 프로세스·스레드와 스레드별 CPU
                                      {"pid", "processes": [{pid, ppid, name, rss_mb, cpu, threads: [{tid, name, cpu}]}]}
  GET /system/workspace              이 보드의 워크스페이스·프로필: 적용한 프로필, 설정 파일 5개(내용·출처),
                                      workspace.env 키별 값·기본값·설명 (profile.py, 비밀값은 가림)
  GET /history/{id}?minutes=60&points=360&keys=a,b&csv=1
                                      지난 지표(기록을 칸 평균)       {"lines": [JSON 또는 CSV 줄...]}
"""
import collections
import glob
import os
import re
import subprocess
import sys

ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")
KEYS_RE = re.compile(r"^[a-z0-9_.,]*$")
WORKSPACE = os.path.abspath(os.environ.get("WORKSPACE_ROOT") or os.path.join(os.path.dirname(__file__), "..", ".."))

ROUTES = [
    (re.compile(r"^/logs/workers/(?P<id>[^/]+)/?$"), "worker_log"),
    (re.compile(r"^/logs/service/?$"), "service_log"),
    (re.compile(r"^/records/commands/?$"), "commands"),
    (re.compile(r"^/system/processes/?$"), "processes"),
    (re.compile(r"^/system/power_modes/?$"), "power_modes"),
    (re.compile(r"^/system/workspace/?$"), "workspace"),
    (re.compile(r"^/history/(?P<id>[^/]+)/?$"), "history"),
    (re.compile(r"^/workers/(?P<id>[^/]+)/tasks/?$"), "worker_tasks"),
]
_CLK_TCK = os.sysconf("SC_CLK_TCK")
_PAGE = os.sysconf("SC_PAGE_SIZE")
TASK_SAMPLE_S = 0.4   # 스레드별 CPU를 잴 간격
MAX_THREADS = 100     # 프로세스 하나에서 돌려줄 스레드 수 상한(CPU 큰 순)


class BadRequest(Exception):
    pass


def _int(params, key, default, lo, hi):
    try:
        return max(lo, min(int(params.get(key, default)), hi))
    except ValueError:
        raise BadRequest("%s must be an integer" % key)


def _tail(path, n):
    """파일 끝 n줄(큰 파일도 끝부분만 읽음)"""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            block, data = 65536, b""
            while size > 0 and data.count(b"\n") <= n:
                step = min(block, size)
                size -= step
                f.seek(size)
                data = f.read(step) + data
    except FileNotFoundError:
        return []
    return data.decode("utf-8", "replace").splitlines()[-n:]


def _stat(path):
    """/proc/.../stat → (comm, 필드 목록: [0]이 3번째 필드 state). 없으면 None"""
    try:
        with open(path, encoding="ascii", errors="replace") as f:
            data = f.read()
    except OSError:
        return None
    return data[data.find("(") + 1:data.rfind(")")], data[data.rfind(")") + 2:].split()


def _group_tasks(pgid):
    """프로세스 그룹(워커와 자식)의 프로세스·스레드. 스레드 CPU는 TASK_SAMPLE_S 동안의 사용률(코어 1개 = 100)"""
    def snapshot():
        procs = {}
        for name in os.listdir("/proc"):
            if not name.isdigit():
                continue
            st = _stat("/proc/%s/stat" % name)
            if st is None or len(st[1]) < 22 or st[1][2] != str(pgid):
                continue
            threads = {}
            try:
                tids = os.listdir("/proc/%s/task" % name)
            except OSError:
                tids = []
            for tid in tids:
                ts = _stat("/proc/%s/task/%s/stat" % (name, tid))
                if ts is not None and len(ts[1]) > 12:
                    threads[int(tid)] = (ts[0], int(ts[1][11]) + int(ts[1][12]))
            procs[int(name)] = {"name": st[0], "ppid": int(st[1][1]), "rss_pages": int(st[1][21]), "threads": threads}
        return procs

    import time
    a = snapshot()
    time.sleep(TASK_SAMPLE_S)
    b = snapshot()
    out = []
    for pid, p in sorted(b.items()):
        prev = a.get(pid, {}).get("threads", {})
        threads = []
        for tid, (tname, ticks) in p["threads"].items():
            cpu = None
            if tid in prev:
                cpu = round(max(ticks - prev[tid][1], 0) / _CLK_TCK / TASK_SAMPLE_S * 100, 1)
            threads.append({"tid": tid, "name": tname, "cpu": cpu})
        threads.sort(key=lambda t: -(t["cpu"] or 0))
        out.append({"pid": pid, "ppid": p["ppid"], "name": p["name"],
                    "rss_mb": round(p["rss_pages"] * _PAGE / 2 ** 20, 1),
                    "cpu": round(sum(t["cpu"] or 0 for t in threads), 1),
                    "thread_count": len(threads), "threads": threads[:MAX_THREADS]})
    return out


def handle(name, match, params, data_dir):
    """(상태 코드, 응답 객체). 잘못된 요청은 BadRequest"""
    wid = match.groupdict().get("id")
    if wid is not None and not ID_RE.match(wid):
        raise BadRequest("bad worker id")

    if name == "workspace":
        from . import profile
        return 200, profile.info(WORKSPACE, data_dir)

    if name == "worker_log":
        return 200, {"lines": _tail(os.path.join(data_dir, "state", "logs", wid + ".log"), _int(params, "lines", 200, 1, 5000))}

    if name == "service_log":
        n = _int(params, "lines", 200, 1, 5000)
        try:
            p = subprocess.run(["journalctl", "-u", "workspace", "-n", str(n), "--no-pager", "-o", "short-iso"],
                               capture_output=True, text=True, errors="replace", timeout=10)
            return 200, {"lines": (p.stdout or p.stderr).splitlines()}
        except (OSError, subprocess.TimeoutExpired) as e:
            return 503, {"error": {"code": "unavailable", "message": "journalctl: %s" % e}}

    if name == "commands":
        # 최근 기록 파일 3개에서 명령 기록만
        n = _int(params, "lines", 200, 1, 5000)
        out = collections.deque(maxlen=n)
        for path in sorted(glob.glob(os.path.join(data_dir, "records", "*.jsonl")))[-3:]:
            try:
                with open(path, encoding="utf-8", errors="replace") as f:
                    for line in f:
                        if '"type":"command"' in line:
                            out.append(line.rstrip("\n"))
            except OSError:
                continue
        return 200, {"lines": list(out)}

    if name == "processes":
        n = _int(params, "lines", 12, 1, 50)
        p = subprocess.run(["ps", "-eo", "pid,comm,%cpu,%mem,rss", "--sort=-%cpu", "--no-headers"],
                           capture_output=True, text=True, errors="replace", timeout=5)
        procs = []
        for line in p.stdout.splitlines()[:n]:
            parts = line.split()
            if len(parts) >= 5:
                procs.append({"pid": int(parts[0]), "name": " ".join(parts[1:-3]), "cpu": float(parts[-3]),
                              "mem": float(parts[-2]), "rss_kb": int(parts[-1])})
        return 200, {"processes": procs}

    if name == "power_modes":
        modes = {}
        try:
            with open("/etc/nvpmodel.conf", encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = re.match(r"^< POWER_MODEL ID=(\d+) NAME=(\S+) >", line)
                    if m:
                        modes[m.group(1)] = m.group(2)
        except OSError:
            pass
        return 200, {"modes": modes}

    if name == "worker_tasks":
        pgid = _int(params, "pid", 0, 0, 2 ** 22)
        if pgid <= 1:
            raise BadRequest("pid is required (worker pid)")
        return 200, {"pid": pgid, "processes": _group_tasks(pgid)}

    if name == "history":
        try:
            minutes = max(1.0, min(float(params.get("minutes", 60)), 7 * 24 * 60))
        except ValueError:
            raise BadRequest("minutes must be a number")
        points = _int(params, "points", 360, 0, 5000)
        keys_raw = params.get("keys", "")
        if not KEYS_RE.match(keys_raw):
            raise BadRequest("bad keys")
        keys = [k for k in keys_raw.split(",") if k] or None
        sys.path.insert(0, os.path.join(WORKSPACE, "logger"))
        try:
            import workspace_history
        finally:
            sys.path.pop(0)
        rows = workspace_history.query(os.path.join(data_dir, "records"), wid, minutes, points, keys)
        return 200, {"lines": workspace_history.render(rows, keys, params.get("csv") == "1")}

    raise KeyError(name)
