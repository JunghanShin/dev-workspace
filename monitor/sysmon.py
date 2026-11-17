#!/usr/bin/env python3
"""보드 상태 감시 워커 (type: sysmon, role: standalone). design.md 6.9

Manager가 실행한다:
    sysmon.py --config <데이터>/state/workers/{id}.json

JetPack 기본 도구 tegrastats의 출력을 interval마다 읽어 heartbeat 지표로 보고한다.
판정하지 않는다. 기록은 workspace-logger, 표시는 REST·UI가 한다.

tegrastats 출처 (지표 collector_root로 표시):
  1 = root 수집기: workspace-start.sh가 root로 돌리는 tegrastats의 최신 한 줄(<데이터>/run/tegrastats.line).
      EMC·NVENC·VIC·DLA 항목이 나온다. 시작할 때 이 파일이 최신이면 이것을 쓴다
  0 = 직접 실행: 이 워커가 tegrastats를 일반 계정으로 실행. EMC·전용 HW 항목은 빠진다
  수집기 파일이 interval의 3배 넘게 바뀌지 않으면 exit 2. Manager가 다시 띄우면 직접 실행으로 바뀐다

워커 계약 (design.md 9장):
  - 첫 줄을 읽으면 READY, 이후 줄마다 WATCHDOG(progress=줄 수 + 지표)
  - SIGTERM 시 tegrastats 정리 후 exit 0
  - exit 1 설정 오류 / 2 tegrastats 없음·비정상 종료·수집기 멈춤 (재시작으로 낫는 경우라 스스로 종료)
  - 자체 워치독(design.md 6.6): 처리 루프가 watchdog_sec + interval 넘게 돌지 않으면
    (tegrastats가 출력 없이 멈춰 읽기가 묶이는 등) 커널이 SIGALRM으로 종료한다

config 키:
  interval_ms   측정 주기 (기본 1000)
  watchdog_sec  자체 워치독 여유 (기본 3). 실제 만료 = watchdog_sec + interval

지표 (tegrastats에 있는 것만. 보드·JetPack 버전에 따라 다를 수 있다):
  ram_used_mb ram_total_mb swap_used_mb
  cpu_avg_pct cpu_max_pct cpu_online   온라인 코어 평균·최대 사용률, 온라인 코어 수
  cpu_mhz_max                          온라인 코어 최고 클럭
  emc_pct                              메모리 대역폭 사용률
  gpu_pct                              GPU(GR3D) 사용률
  nvenc_mhz nvdec_mhz nvjpg_mhz vic_mhz nvdla0_mhz nvdla1_mhz ofa_mhz
                                       전용 HW 클럭. 0이면 꺼짐(사용 안 함)
  temp_<부위>_c temp_max_c             온도
  power_<레일>_mw                      레일별 현재 전력(VDD_IN = 보드 전체 입력)
  power_mode                           nvpmodel 모드 번호(읽을 수 있을 때)

추가 지표 (tegrastats 밖, /proc·/sys에서. jtop 수준):
  cpu<N>_pct cpu<N>_mhz                코어별 사용률·클럭(꺼진 코어는 빠짐)
  gpu_mhz                              GPU(GR3D) 클럭
  swap_total_mb
  disk_used_gb disk_total_gb           / 파일 시스템
  net_rx_kbps net_tx_kbps              기본 경로 인터페이스의 수신·송신(직전 표본 대비)
  fan_pct fan_rpm                      팬 PWM(0~100%)·회전수(장치가 있을 때)
  uptime_s                             가동 시간
"""
import glob
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

import workspace_worker as ww  # 워커 계약 도우미(사본, 원본 workers/_template/python/)

EXIT_OK = ww.EXIT_OK
EXIT_CONFIG = ww.EXIT_CONFIG
EXIT_DEVICE = ww.EXIT_DEVICE

CONFIG_SCHEMA = {"interval_ms": (int, 1000), "watchdog_sec": (float, 3.0)}
FIRST_LINE_TIMEOUT_S = 5.0
STALE_FACTOR = 3  # 수집기 파일이 interval의 이 배수 넘게 안 바뀌면 멈춘 것으로 본다

_RAM = re.compile(r"\bRAM (\d+)/(\d+)MB")
_SWAP = re.compile(r"\bSWAP (\d+)/(\d+)MB")
_CPU = re.compile(r"\bCPU \[([^\]]*)\]")
_CORE = re.compile(r"(\d+)%@(\d+)")
_EMC = re.compile(r"\bEMC_FREQ (\d+)%")
_GPU = re.compile(r"\bGR3D_FREQ (\d+)%")
_GPU_MHZ = re.compile(r"\bGR3D_FREQ \d+%@\[?(\d+)")
# NVENC 1036 / NVENC off / VIC_FREQ 0%@115 / NVDLA0_FREQ @1600 등 버전별 표기를 모두 받는다
_ENGINE = re.compile(r"\b(NVENC|NVDEC|NVJPG|VIC|NVDLA0|NVDLA1|OFA)(?:_FREQ)? "
                     r"(off|\d+%@\[?(\d+)|@\[?(\d+)|(\d+))")
_TEMP = re.compile(r"\b([A-Za-z][\w]*)@(-?\d+(?:\.\d+)?)C\b")
_POWER = re.compile(r"\b((?:VDD|VIN)_\w+) (\d+)mW/\d+mW")


ConfigError = ww.ConfigError


class Stale(Exception):
    pass


def _key(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def parse(line):
    """tegrastats 한 줄 → {지표: 숫자}"""
    m = {}
    if (r := _RAM.search(line)):
        m["ram_used_mb"], m["ram_total_mb"] = int(r[1]), int(r[2])
    if (r := _SWAP.search(line)):
        m["swap_used_mb"], m["swap_total_mb"] = int(r[1]), int(r[2])
    if (r := _CPU.search(line)):
        cores = [(int(p), int(f)) for p, f in _CORE.findall(r[1])]
        # 코어별: "11%@1497" 또는 "off"(꺼진 코어는 빠진다)
        for i, entry in enumerate(r[1].split(",")):
            if (c := _CORE.fullmatch(entry.strip())):
                m["cpu%d_pct" % i], m["cpu%d_mhz" % i] = int(c[1]), int(c[2])
        if cores:
            m["cpu_online"] = len(cores)
            m["cpu_avg_pct"] = round(sum(p for p, _ in cores) / len(cores), 1)
            m["cpu_max_pct"] = max(p for p, _ in cores)
            m["cpu_mhz_max"] = max(f for _, f in cores)
    if (r := _EMC.search(line)):
        m["emc_pct"] = int(r[1])
    if (r := _GPU.search(line)):
        m["gpu_pct"] = int(r[1])
    if (r := _GPU_MHZ.search(line)):
        m["gpu_mhz"] = int(r[1])
    for name, value, a, b, c in _ENGINE.findall(line):
        m["%s_mhz" % _key(name)] = 0 if value == "off" else int(a or b or c)
    temps = {}
    for name, value in _TEMP.findall(line):
        temps["temp_%s_c" % _key(name)] = float(value)
    # tegrastats는 센서가 없을 때 -256C를 낸다
    temps = {k: v for k, v in temps.items() if v > -200}
    if temps:
        m.update(temps)
        m["temp_max_c"] = max(temps.values())
    for name, value in _POWER.findall(line):
        m["power_%s_mw" % _key(name)] = int(value)
    return m


class SystemExtras:
    """tegrastats에 없는 지표: 디스크, 네트워크(직전 표본 대비), 팬, 가동 시간. 읽지 못하는 항목은 빠진다"""

    def __init__(self):
        self.prev_net = None
        self.fan_pwm = self.fan_rpm = None
        for hw in glob.glob("/sys/class/hwmon/hwmon*"):
            name = self._read(hw + "/name")
            if name == "pwmfan" and os.path.exists(hw + "/pwm1"):
                self.fan_pwm = hw + "/pwm1"
            elif name == "pwm_tach" and os.path.exists(hw + "/rpm"):
                self.fan_rpm = hw + "/rpm"

    @staticmethod
    def _read(path):
        try:
            with open(path, encoding="ascii") as f:
                return f.read().strip()
        except OSError:
            return None

    @staticmethod
    def default_iface():
        try:
            with open("/proc/net/route", encoding="ascii") as f:
                rows = [l.split() for l in f.readlines()[1:]]
        except OSError:
            return None
        best = None
        for r in rows:
            if len(r) > 6 and r[1] == "00000000" and int(r[3], 16) & 1:  # 기본 경로, UP
                if best is None or int(r[6]) < int(best[6]):
                    best = r
        return best[0] if best else None

    def sample(self):
        m = {}
        try:
            st = os.statvfs("/")
            m["disk_total_gb"] = round(st.f_blocks * st.f_frsize / 1e9, 1)
            m["disk_used_gb"] = round((st.f_blocks - st.f_bfree) * st.f_frsize / 1e9, 1)
        except OSError:
            pass
        iface = self.default_iface()
        if iface:
            rx = self._read("/sys/class/net/%s/statistics/rx_bytes" % iface)
            tx = self._read("/sys/class/net/%s/statistics/tx_bytes" % iface)
            now = time.monotonic()
            if rx and tx:
                cur = (iface, now, int(rx), int(tx))
                prev = self.prev_net
                if prev and prev[0] == iface and now > prev[1]:
                    dt = now - prev[1]
                    m["net_rx_kbps"] = round((cur[2] - prev[2]) * 8 / 1000 / dt, 1)
                    m["net_tx_kbps"] = round((cur[3] - prev[3]) * 8 / 1000 / dt, 1)
                self.prev_net = cur
        if self.fan_pwm and (v := self._read(self.fan_pwm)) and v.isdigit():
            m["fan_pct"] = round(int(v) * 100 / 255)
        if self.fan_rpm and (v := self._read(self.fan_rpm)) and v.lstrip("-").isdigit():
            m["fan_rpm"] = int(v)
        up = self._read("/proc/uptime")
        if up:
            m["uptime_s"] = int(float(up.split()[0]))
        return m


def power_mode():
    try:
        with open("/var/lib/nvpmodel/status", encoding="ascii") as f:
            return int(f.read().strip().split(":")[-1])
    except (OSError, ValueError):
        return None


def load_config():
    _wid, cfg = ww.load_config(schema=CONFIG_SCHEMA, description="워크스페이스 sysmon worker")
    if cfg["interval_ms"] < 100:
        raise ConfigError("interval_ms must be >= 100")
    if cfg["watchdog_sec"] <= 0:
        raise ConfigError("watchdog_sec must be positive")
    print("config: %s" % cfg, flush=True)
    return cfg


Watchdog = ww.Watchdog
Heartbeat = ww.Heartbeat


def collector_path():
    data = os.environ.get("WORKSPACE_DATA")
    return os.path.join(data, "run", "tegrastats.line") if data else None


def fresh(path, max_age_s):
    try:
        return time.time() - os.path.getmtime(path) <= max_age_s
    except OSError:
        return False


def lines_from_collector(path, interval_s, stopping, wd):
    """root 수집기 파일에서 새 줄이 생길 때마다 하나씩. 멈추면 Stale"""
    last = None
    changed = time.monotonic()
    while not stopping:
        wd.kick()  # 기다리는 동안도 이 루프가 처리 루프다
        try:
            mtime = os.stat(path).st_mtime_ns
        except OSError:
            mtime = None
        now = time.monotonic()
        if mtime is not None and mtime != last:
            last, changed = mtime, now
            try:
                with open(path, encoding="utf-8", errors="replace") as f:
                    line = f.read()
            except OSError:
                line = ""
            if line:
                yield line
        elif now - changed > interval_s * STALE_FACTOR:
            raise Stale("collector %s not updated for %.1fs" % (path, now - changed))
        time.sleep(interval_s / 4)


def run(cfg):
    interval_s = cfg["interval_ms"] / 1000
    path = collector_path()
    stopping = []
    proc = None
    # 만료 = 여유 + 한 주기(다음 줄을 기다리는 시간). 첫 줄은 tegrastats 시작까지 더 기다린다
    wd = Watchdog(cfg["watchdog_sec"] + interval_s)
    wd.kick(FIRST_LINE_TIMEOUT_S + interval_s + cfg["watchdog_sec"])
    if path and fresh(path, interval_s * STALE_FACTOR):
        root = 1
        lines = lines_from_collector(path, interval_s, stopping, wd)
        print("source: root collector %s" % path, flush=True)
    else:
        binary = shutil.which("tegrastats")
        if not binary:
            print("tegrastats not found (not a Jetson / JetPack missing?)",
                  file=sys.stderr, flush=True)
            return EXIT_DEVICE
        root = 0
        proc = subprocess.Popen([binary, "--interval", str(cfg["interval_ms"])],
                                stdout=subprocess.PIPE, text=True, bufsize=1)
        lines = proc.stdout
        print("source: own tegrastats (no root collector; EMC/HW engine values unavailable)",
              flush=True)

    def on_term(_sig, _frame):
        stopping.append(True)
        if proc is not None:
            proc.terminate()  # readline을 깨운다

    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)
    hb = Heartbeat()
    mode = power_mode()
    extras = SystemExtras()
    count = 0
    deadline = time.monotonic() + FIRST_LINE_TIMEOUT_S + interval_s
    try:
        for line in lines:
            wd.kick()
            metrics = parse(line)
            if not metrics:
                if count == 0 and time.monotonic() > deadline:
                    print("tegrastats output not recognized: %r" % line, file=sys.stderr, flush=True)
                    return EXIT_DEVICE
                continue
            if mode is not None:
                metrics["power_mode"] = mode
            metrics.update(extras.sample())
            metrics["collector_root"] = root
            count += 1
            if count == 1:
                hb.ready()
                print("first sample: %s" % metrics, flush=True)
            hb.send("WATCHDOG=1 progress=%d %s"
                    % (count, " ".join("%s=%s" % kv for kv in metrics.items())))
        if stopping:
            return EXIT_OK
        if proc is not None:
            print("tegrastats exited (%s)" % proc.wait(), file=sys.stderr, flush=True)
        return EXIT_DEVICE
    except Stale as e:
        print("%s; exiting so the Manager restarts with own tegrastats" % e,
              file=sys.stderr, flush=True)
        return EXIT_DEVICE
    finally:
        wd.kick(5.0)  # 정리(tegrastats 종료 대기 최대 2초) 중에 만료되지 않게
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()


def main():
    try:
        cfg = load_config()
    except ConfigError as e:
        print("config error: %s" % e, file=sys.stderr, flush=True)
        return EXIT_CONFIG
    return run(cfg)


if __name__ == "__main__":
    sys.exit(main())
