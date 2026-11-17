"""워커별 CPU·메모리·프로세스 수·스레드 수 (design.md 9.5). 워커는 자기 세션의 그룹 리더이므로
프로세스 그룹(pgid = 워커 pid) 단위로 합산한다. ffmpeg 같은 자식도 포함된다.
"""
import os
import time

_CLK_TCK = os.sysconf("SC_CLK_TCK")
_PAGE = os.sysconf("SC_PAGE_SIZE")


def _read_groups(pgids):
    """{pgid: [cpu ticks, rss pages, 프로세스 수, 스레드 수]}"""
    totals = {g: [0, 0, 0, 0] for g in pgids}
    if not totals:
        return totals
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % name, encoding="ascii", errors="replace") as f:
                data = f.read()
        except OSError:
            continue
        # comm에 공백·괄호가 있을 수 있어 마지막 ')' 뒤부터 센다. rest[0]이 3번째 필드(state)
        rest = data[data.rfind(")") + 2:].split()
        try:
            group = totals.get(int(rest[2]))           # 5: pgrp
            if group is None:
                continue
            group[0] += int(rest[11]) + int(rest[12])  # 14: utime, 15: stime
            group[1] += int(rest[21])                  # 24: rss
            group[2] += 1
            group[3] += int(rest[17])                  # 20: num_threads
        except (IndexError, ValueError):
            continue
    return totals


class ResourceSampler:
    def __init__(self):
        self._prev = {}  # pgid -> (ticks, monotonic)

    def sample(self, pgids):
        """{pgid: {"cpu_pct", "rss_mb", "procs", "threads"}}. cpu_pct는 두 번째 표본부터(코어 1개 = 100)."""
        now = time.monotonic()
        out = {}
        for pgid, (ticks, rss, procs, threads) in _read_groups(pgids).items():
            prev = self._prev.get(pgid)
            cpu = None
            if prev is not None and now > prev[1]:
                cpu = round(max(ticks - prev[0], 0) / _CLK_TCK / (now - prev[1]) * 100, 1)
            self._prev[pgid] = (ticks, now)
            out[pgid] = {"cpu_pct": cpu, "rss_mb": round(rss * _PAGE / 2 ** 20, 1),
                         "procs": procs, "threads": threads}
        for pgid in set(self._prev) - set(out):
            del self._prev[pgid]
        return out
