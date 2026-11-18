#!/bin/bash
# 시스템 시험: 보드에서 워크스페이스가 돌고 있을 때 전체 상태를 한 번에 점검한다(워크스페이스 design.md 11.3).
#
#   dev/system-check.sh                 이 보드(http://127.0.0.1:8080)
#   dev/system-check.sh http://<주소>:<포트>
#   WORKSPACE_TOKEN=<토큰> dev/system-check.sh   제품 모드(REST 토큰)
#
# 보는 것(REST로만, 보드를 바꾸지 않음):
#   - REST·Manager 응답, Manager 클라이언트 접속
#   - 워커마다: 실행 상태(목표대로인지), heartbeat 나이, 재시작 수, 마지막 오류, 계약 버전(libworker), 가짜 장치·개발 빌드 표시
#   - 장치·상대 연결: 지표 *_link 또는 *_up이 1인지
#   - 워커 간 연결: 설정이 가리키는 상대 워커(peers)가 있고 돌고 있는지
#   - 텔레메트리 스트림: 발행하는 워커의 스트림에서 첫 메시지를 받는지
# 결과는 표로, 하나라도 FAIL이면 종료 코드 1. WARN은 알림
set -uo pipefail
BASE="${1:-http://127.0.0.1:8080}"
exec python3 - "$BASE" <<'PY'
import json, os, sys, time, urllib.request

BASE = sys.argv[1].rstrip("/")
rows, fails = [], 0


TOKEN = os.environ.get("WORKSPACE_TOKEN", "")  # 제품 모드(REST 토큰)면: WORKSPACE_TOKEN=<토큰> dev/system-check.sh


def req(path):
    r = urllib.request.Request(BASE + path)
    if TOKEN:
        r.add_header("Authorization", "Bearer " + TOKEN)
    return r


def get(path, timeout=5):
    with urllib.request.urlopen(req(path), timeout=timeout) as r:
        return json.loads(r.read().decode())


def add(level, what, detail=""):
    global fails
    rows.append((level, what, detail))
    if level == "FAIL":
        fails += 1


try:
    h = get("/health")
    add("OK" if h.get("manager") == "ok" else "FAIL", "REST·Manager", "manager=%s" % h.get("manager"))
    sysst = get("/system")
    add("OK", "Manager 클라이언트", ", ".join("%s:%s" % kv for kv in (sysst.get("clients") or {}).items()))
    workers = get("/workers")
except Exception as e:
    print("REST에 닿지 않음(%s): %s" % (BASE, e))
    sys.exit(1)

ids = {w["id"]: w for w in workers}
versions = {}
for w in workers:
    wid, st, want = w["id"], w["actual_state"], w["desired_state"]
    if st == "disabled":
        add("OK", wid, "사용 안 함(%s)" % (w.get("last_error") or ""))
        continue
    if want == "running" and st != "running":
        add("FAIL", wid, "목표 running, 지금 %s (%s)" % (st, w.get("last_error") or "-"))
        continue
    if want == "stopped":
        add("OK", wid, "정지(목표대로)")
        continue
    hb = w.get("last_heartbeat_ms_ago")
    detail = "실행, heartbeat %s ms 전, 재시작 %s, 프리셋 %s%s%s%s" % (hb, w.get("restart_count"), w.get("preset"),
                                                                  " (임시 적용 중)" if w.get("temp") else "",
                                                                  " (가짜 장치 모드)" if w.get("sim") else "",
                                                                  " (개발 빌드)" if "/build/" in (w.get("exec") or "") else "")
    add("OK" if hb is not None and hb < 3000 else "FAIL", wid, detail)
    if w.get("libworker") and not str(w["libworker"]).startswith("py-"):  # Python 도우미(py-)는 다른 구현
        versions.setdefault(w["libworker"], []).append(wid)
    for k, v in sorted((w.get("metrics") or {}).items()):
        if k.endswith("_link") or k.endswith("_up"):  # 연결 지표(0/1). 모듈 이름이 link면 link_up
            add("OK" if v and float(v) >= 1 else "WARN", "  %s %s" % (wid, k), "연결" if v and float(v) >= 1 else "끊김(장치·상대 확인)")
    for path, peer in sorted((w.get("peers") or {}).items()):
        p = ids.get(peer)
        if p is None:
            add("FAIL", "  %s → %s" % (wid, peer), "설정 %s가 가리키는 워커가 없음" % path)
        elif p["actual_state"] != "running":
            add("WARN", "  %s → %s" % (wid, peer), "상대가 %s" % p["actual_state"])
        else:
            add("OK", "  %s → %s" % (wid, peer), "상대 실행 중(%s)" % path)
    # 텔레메트리 스트림: 첫 메시지(SSE "data:" 줄)를 3초 안에 받는지. 발행하지 않는 워커는 down만 온다
    try:
        with urllib.request.urlopen(req("/workers/%s/stream?hz=1" % wid), timeout=3) as r:
            end, got = time.time() + 3, None
            while time.time() < end:
                try:
                    line = r.readline().decode(errors="replace").strip()
                except OSError:  # 더 오는 것이 없음(발행하지 않는 워커는 down 한 줄뿐)
                    break
                if line.startswith("data:"):
                    got = json.loads(line[5:])
                    if got.get("stream") != "down":  # 중계가 처음 보내는 down 다음에 기준 상태가 온다
                        break
            if got is None:
                add("WARN", "  %s 스트림" % wid, "메시지 없음")
            elif got.get("stream") == "down":
                add("OK", "  %s 스트림" % wid, "발행 안 함(폴링 워커)")
            else:
                add("OK", "  %s 스트림" % wid, "받음(주제 %s)" % ", ".join(sorted((got.get("topics") or {}).keys())))
    except Exception as e:
        add("WARN", "  %s 스트림" % wid, str(e)[:60])

if len(versions) > 1:
    add("WARN", "계약 버전", "libworker 버전이 섞임: " + "; ".join("%s: %s" % (v, ", ".join(ws)) for v, ws in versions.items()))
elif versions:
    add("OK", "계약 버전", "libworker %s" % next(iter(versions)))
if sysst.get("product"):
    add("OK", "제품 모드", "설정 쓰기 %s" % ("허용" if sysst.get("config_write") else "잠금"))

w1 = max(len(r[1]) for r in rows)
for level, what, detail in rows:
    print("%-4s  %-*s  %s" % (level, w1, what, detail))
print("\n%s: FAIL %d, WARN %d" % ("이상 있음" if fails else "정상", fails, sum(1 for r in rows if r[0] == "WARN")))
sys.exit(1 if fails else 0)
PY
