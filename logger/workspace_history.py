#!/usr/bin/env python3
"""workspace-logger 기록(<데이터>/records/*.jsonl)에서 워커 하나의 지표를 기간별로 뽑는다 (design.md 6.9).

REST(GET /history/{id})가 query()·render()를 불러 워크스페이스 UI 지난 기록 그래프·내려받기에 준다. 명령줄로도 쓴다. 보드 파일을 읽기만 한다.

    workspace_history.py --id system-monitor --minutes 60 [--points 360] [--keys cpu_avg_pct,gpu_pct] [--csv]

  --points N  기간을 N칸으로 나눠 칸마다 숫자 지표를 평균(그래프용). 0이면 줄이지 않음(내려받기용)
  --keys      뽑을 지표(쉼표). 없으면 전부
  --csv       CSV(첫 줄 머리). 없으면 한 줄에 {"t": "...", "m": {...}} JSON

기록 한 줄: {"t": "2026-11-05T17:21:48.720+09:00", "type": "worker", "id": ..., "metrics": {...}, ...}
지표에는 워커가 보낸 metrics에 더해 Manager가 잰 자원 res.cpu_pct·res.rss_mb·res.procs·res.threads와 restart_count가 들어간다.
"""
import argparse
import datetime
import glob
import json
import os
import sys


def parse_t(text):
    return datetime.datetime.fromisoformat(text).timestamp()


def query(records_dir, worker_id, minutes=60, points=360, keys=None):
    """[(시각 epoch, {지표: 숫자})]. points > 0이면 기간을 points칸으로 나눠 칸마다 평균(빈 칸은 빠짐)"""
    now = datetime.datetime.now().timestamp()
    since = now - minutes * 60
    needle = '"id":"%s"' % worker_id
    rows = []
    # 파일 이름이 시작 시각 순이다. 기간 안에 쓰인(수정된) 파일만 읽는다
    for path in sorted(glob.glob(os.path.join(records_dir, "*.jsonl"))):
        try:
            if os.path.getmtime(path) < since:
                continue
            with open(path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    if needle not in line or '"type":"worker"' not in line:
                        continue
                    try:
                        rec = json.loads(line)
                        t = parse_t(rec["t"])
                    except (ValueError, KeyError):
                        continue
                    m = rec.get("metrics")
                    if t < since or not isinstance(m, dict):
                        continue
                    # Manager가 잰 워커 자원(자식 프로세스 포함)과 재시작 횟수도 지표처럼(res.*, restart_count)
                    m = dict(m)
                    for k, v in (rec.get("resources") or {}).items():
                        m["res." + k] = v
                    if isinstance(rec.get("restart_count"), int):
                        m["restart_count"] = rec["restart_count"]
                    if keys:
                        m = {k: m[k] for k in keys if k in m}
                    rows.append((t, {k: v for k, v in m.items() if isinstance(v, (int, float))}))
        except OSError:
            continue
    rows.sort(key=lambda r: r[0])
    if points > 0 and len(rows) > points:
        span = (now - since) / points
        buckets = {}
        for t, m in rows:
            b = buckets.setdefault(int((t - since) // span), [{}, {}])
            for k, v in m.items():
                b[0][k] = b[0].get(k, 0) + v
                b[1][k] = b[1].get(k, 0) + 1
        rows = [(since + (i + 0.5) * span, {k: round(s / b[1][k], 2) for k, s in b[0].items()})
                for i, b in sorted(buckets.items())]
    return rows


def render(rows, keys=None, csv=False):
    """줄 목록: CSV(첫 줄 머리) 또는 한 줄에 {"t", "m"} JSON"""
    iso = lambda t: datetime.datetime.fromtimestamp(t).astimezone().isoformat(timespec="seconds")
    if csv:
        cols = keys or sorted({k for _, m in rows for k in m})
        return [",".join(["time"] + cols)] + [
            ",".join([iso(t)] + ["" if m.get(k) is None else str(m[k]) for k in cols]) for t, m in rows]
    return [json.dumps({"t": iso(t), "m": m}, separators=(",", ":")) for t, m in rows]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(os.environ.get("WORKSPACE_DATA") or os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")) + "_data", "records"))
    ap.add_argument("--id", required=True)
    ap.add_argument("--minutes", type=float, default=60)
    ap.add_argument("--points", type=int, default=360)
    ap.add_argument("--keys", default="")
    ap.add_argument("--csv", action="store_true")
    a = ap.parse_args()
    keys = [k for k in a.keys.split(",") if k] or None
    for line in render(query(a.dir, a.id, a.minutes, a.points, keys), keys, a.csv):
        print(line)


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:  # head 등으로 앞부분만 읽을 때
        sys.exit(0)
