"""제품 묶음 만들기(워크스페이스 design.md 11.4). 표준 라이브러리만.

워크스페이스(개발 작업대)에서 개발 부품을 걷어내고 장비에 올릴 결과물 하나를 만든다:
  넣음: 확정 워커 실행 파일(workspace/bin, 소스와 같은지 확인), Manager·게이트웨이·시스템 모니터·매니저(C++ 확정 실행 파일),
        잠근 설정(워커마다 고른 프리셋 하나), 프로필, 시작 셸, 제품 설치 스크립트(서비스·되돌리기),
        기본 화면(셸 + 시스템 모니터 + 워커 패널, product.json ui=true일 때), 전원 모드(설치가 맞춤, power_mode)
  뺌:   소스·깃·문서·배포 키·빌드 도구, 가짜 장치(*-sim, libfake_*), 다른 프리셋, 개발 빌드 후보(build/),
        진단·설정 화면(로그·명령 이력·워크스페이스)
결과: <out>/<이름>-<버전>.tar.gz (풀어서 sudo bash install.sh --owner <계정>)

Python 부분 컴파일(Nuitka)과 심볼 제거는 aarch64에서 해야 한다:
  - 보드에서 실행하면(화면의 "제품 빌드" 버튼 = REST가 --local로 부름) 그 자리에서
  - PC에서 실행하면(dev/build-product.sh) dev/board.env의 WORKSPACE_BOARD에 보내 임시 폴더에서 하고 결과만 가져온다
    (보드의 워크스페이스는 건드리지 않음)

    python3 dev/build_product.py <프로필> [--out dist] [--local] [--replace] [--power-mode <이름|keep>] [--allow-dirty] [--skip-compile]
    python3 dev/build_product.py <프로필> --check [--json] [--local]     # 빌드 전 점검만
    python3 dev/build_product.py --profiles                              # product.json이 있는 프로필 목록(JSON)

진행 상황은 표준 출력 줄 "##PROGRESS <0~100> <글>", 끝나면 "##RESULT <JSON>", 실패하면 "##ERROR <글>"(화면이 읽음).
"""
import argparse
import glob
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
APPS = {  # app/<모듈>/<모듈>: 시작 셸이 python3 -m <모듈> 대신 쓴다(workspace-start.sh run_as_owner)
    # Manager·게이트웨이·시스템 모니터·매니저는 C++ 확정 실행 파일을 그대로 넣는다(PLATFORM_BINS).
    # 예전 workspace_logger는 record-manager가 대신한다(managers/record). 지금 컴파일할 Python 앱은 없다
}
PLATFORM_BINS = {"manager/bin": ["process-manager", "BUILD"], "gateway/bin": ["gateway", "BUILD"],
                 "monitor/bin": ["sysmon", "BUILD"], "managers/record/bin": ["record-manager", "BUILD"],
                 "managers/status/bin": ["status-manager", "BUILD"], "managers/mission/bin": ["mission-manager", "BUILD"],
                 "managers/payload/bin": ["payload-manager", "BUILD"], "managers/fault/bin": ["fault-manager", "BUILD"]}  # C++ 확정 실행 파일(release-worker.sh)
DEV_ONLY = ("-sim", "libfake_")  # 가짜 장치 부품(제품에서 뺌)
DEPS = "$HOME/.cache/workspace-product/nuitka-deps"  # 컴파일 도구(Nuitka) 받아 두는 곳(컴파일하는 계정의 홈)
MIN_FREE_GB = 1.5


class BuildError(Exception):
    pass


def progress(pct, msg):
    print("##PROGRESS %d %s" % (pct, msg), flush=True)


def run(cmd, **kw):
    print("+ " + (cmd if isinstance(cmd, str) else " ".join(cmd)), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), check=True, capture_output=True, text=True).stdout.strip()


def board_env():
    env = {}
    p = os.path.join(ROOT, "dev", "board.env")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip().strip("'\"")
    env.update({k: v for k, v in os.environ.items() if k.startswith("WORKSPACE_BOARD")})
    return env


def profiles():
    out = []
    for pj in sorted(glob.glob(os.path.join(ROOT, "install", "profiles", "*", "product.json"))):
        try:
            p = json.load(open(pj, encoding="utf-8"))
        except ValueError:
            continue
        out.append({"profile": os.path.basename(os.path.dirname(pj)), "name": p.get("name"),
                    "workers": sorted(p.get("workers", {})), "ui": bool(p.get("ui")),
                    "power_mode": p.get("power_mode") or "keep"})
    return out


def load_product(profile):
    pj = os.path.join(ROOT, "install", "profiles", profile, "product.json")
    if not os.path.exists(pj):
        raise BuildError("no product.json for profile %s (install/profiles/%s/product.json)" % (profile, profile))
    product = json.load(open(pj, encoding="utf-8"))
    product["_profile"] = profile
    return product


class Runner:
    """컴파일할 곳: 이 보드(local) 또는 PC에서 ssh로 보드. sh(명령)은 그곳의 bash에서 실행"""

    def __init__(self, local, benv):
        self.local = local
        if local:
            self.ssh, self.where = None, "this board"
        else:
            host = benv.get("WORKSPACE_BOARD")
            if not host:
                raise BuildError("set WORKSPACE_BOARD (dev/board.env) to an aarch64 board for compiling (Nuitka)")
            self.ssh = ["ssh", "-p", benv.get("WORKSPACE_BOARD_PORT", "22"), "-o", "BatchMode=yes",
                        "-o", "ConnectTimeout=10", host]
            self.where = host

    def sh(self, cmd, **kw):
        return run(self.ssh + [cmd] if self.ssh else ["bash", "-c", cmd], **kw)


def is_local(arg_local, benv):
    return arg_local or (platform.machine() in ("aarch64", "arm64") and not benv.get("WORKSPACE_BOARD"))


# ---- 빌드 전 점검(화면 1단계와 같은 것) ------------------------------------------------------------

def check(product, local, allow_dirty=False, out=None):
    """[{key, label, ok, detail}] — 하나라도 ok=False면 빌드하지 않는다. info는 워커 정보(빌드가 씀)"""
    items, info = [], {}

    def add(key, label, ok, detail=""):
        items.append({"key": key, "label": label, "ok": bool(ok), "detail": detail})

    porcelain = subprocess.run(["git", "-C", ROOT, "status", "--porcelain"], check=True, capture_output=True, text=True).stdout
    dirty = [l[3:] for l in porcelain.splitlines() if l.strip()]  # "XY 경로"(앞 공백을 지우면 안 됨)
    add("git", "깃 정리됨(커밋한 그대로 빌드)", not dirty or allow_dirty,
        "" if not dirty else "안 올린 파일 %d개: %s%s%s" % (len(dirty), ", ".join(dirty[:4]), " …" if len(dirty) > 4 else "",
                                                       " (--allow-dirty)" if allow_dirty else ". 커밋한 뒤 빌드"))
    for w, opt in sorted(product.get("workers", {}).items()):
        wdir = os.path.join(ROOT, "workers", w)
        build = os.path.join(wdir, "workspace", "bin", "BUILD")
        preset_name = opt.get("preset", "default")
        if not os.path.exists(build):
            add("worker:" + w, "%s 확정본" % w, False, "확정 실행 파일 없음: dev/release-worker.sh %s" % w)
            continue
        src = next((l.split(":", 1)[1].strip() for l in open(build, encoding="utf-8") if l.startswith("source_commit:")), "")
        changed = subprocess.run(["git", "-C", ROOT, "diff", "--quiet", src, "HEAD", "--", "workers/%s" % w,
                                  ":(exclude)workers/%s/workspace" % w, ":(exclude,glob)workers/%s/**/*.md" % w],
                                 capture_output=True).returncode  # 문서(.md)는 실행 파일과 무관
        preset = os.path.join(wdir, "workspace", "presets", preset_name + ".json")
        problem = ""
        if changed and not allow_dirty:
            problem = "확정(%s) 뒤 소스가 바뀜: dev/release-worker.sh %s" % (src, w)
        elif not os.path.exists(preset):
            problem = "프리셋 %s 없음" % preset_name
        elif "sim" in json.load(open(preset, encoding="utf-8")).get("config", {}):
            problem = "프리셋 %s는 가짜 장치(sim) 프리셋" % preset_name
        add("worker:" + w, "%s 확정본" % w, not problem, problem or "소스 %s · 프리셋 %s" % (src, preset_name))
        info[w] = {"preset": preset_name, "autostart": bool(opt.get("autostart", True)), "source_commit": src}
    if local:
        have = os.path.exists(os.path.expandvars(DEPS + "/bin/nuitka"))
        tools = [t for t in ("gcc", "strip") if not shutil.which(t)]  # patchelf는 Nuitka와 함께 DEPS에 받는다
        pip = subprocess.run([sys.executable, "-m", "pip", "--version"], capture_output=True).returncode == 0
        ok = not tools and (have or pip)
        add("tools", "컴파일 도구(Nuitka·gcc·strip)", ok,
            ("없음: " + ", ".join(tools)) if tools else ("준비됨" if have else
                                                        ("첫 빌드 때 받음(인터넷 필요, 1~2분)" if pip else "pip 없음: sudo apt install python3-pip")))
    else:
        b = board_env().get("WORKSPACE_BOARD")
        add("tools", "컴파일할 보드(WORKSPACE_BOARD)", bool(b), b or "dev/board.env에 WORKSPACE_BOARD")
    out = out or os.path.join(ROOT, "dist")
    os.makedirs(out, exist_ok=True)
    free = shutil.disk_usage(out).free / 1e9
    add("disk", "빈 공간", free >= MIN_FREE_GB, "%.1f GB (%s)" % (free, out))
    return items, info


# ---- 묶음 만들기 ---------------------------------------------------------------------------------

def stage_tree(product, info, stage, version):
    def cp(rel, dst_rel=None):
        s, d = os.path.join(ROOT, rel), os.path.join(stage, dst_rel or rel)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        if os.path.isdir(s):
            shutil.copytree(s, d, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "build", "build*"))
        else:
            shutil.copy2(s, d)

    for f in ("workspace-start.sh", "workspace-stop.sh"):
        cp(f)
    cp("install/config")
    # Manager·게이트웨이(C++ 확정 실행 파일)와 nginx 설정 틀(시작 셸이 데이터 폴더에 설정을 만듦)
    for d, names in PLATFORM_BINS.items():
        for n in names:
            if not os.path.exists(os.path.join(ROOT, d, n)):
                raise BuildError("%s/%s 없음: dev/release-worker.sh %s 로 확정하세요" % (d, n, d.split("/")[-2]))
            cp(os.path.join(d, n))
    cp("gateway/nginx")
    for f in ("workspace-network-apply", "workspace-tegrastats-collect", "workspace-power-mode"):
        cp("install/bin/" + f)
    # 프로필: 장비 config로 처음 한 번 들어간다(install.sh). product.json·문서는 넣지 않음
    pdir = os.path.join(ROOT, "install", "profiles", product["_profile"])
    for f in sorted(os.listdir(pdir)):
        if f in ("README.md", "product.json", "docs") or os.path.isdir(os.path.join(pdir, f)):
            continue
        cp(os.path.join("install", "profiles", product["_profile"], f), os.path.join("profile", f))
    # 워커: workspace/만(설치·준비·type·확정 실행 파일·고른 프리셋). 가짜 장치·다른 프리셋·화면(선택) 뺌
    for w, i in info.items():
        src = os.path.join(ROOT, "workers", w, "workspace")
        dst = os.path.join(stage, "workers", w, "workspace")
        os.makedirs(dst, exist_ok=True)
        for name in os.listdir(src):
            if name in ("presets", "config.json") or (name == "panel" and not product.get("ui")):
                continue
            s = os.path.join(src, name)
            if name == "bin":
                os.makedirs(os.path.join(dst, "bin"))
                for b in os.listdir(s):
                    if not any(t in b for t in DEV_ONLY):
                        shutil.copy2(os.path.join(s, b), os.path.join(dst, "bin", b))
            elif os.path.isdir(s):
                shutil.copytree(s, os.path.join(dst, name))
            else:
                shutil.copy2(s, os.path.join(dst, name))
        os.makedirs(os.path.join(dst, "presets"))
        shutil.copy2(os.path.join(src, "presets", i["preset"] + ".json"), os.path.join(dst, "presets", i["preset"] + ".json"))
        with open(os.path.join(dst, "config.json"), "w", encoding="utf-8") as f:
            json.dump({"active": i["preset"], "autostart": i["autostart"]}, f, indent=2)
        # 입출력 정의(design.md 6.6.7.1): 보드가 계약을 내줌(REST GET /workers/{id}/io). io.json, io/, self.json의 include 조각
        wdir = os.path.join(ROOT, "workers", w)
        if os.path.exists(os.path.join(wdir, "io.json")):
            shutil.copy2(os.path.join(wdir, "io.json"), os.path.join(stage, "workers", w, "io.json"))
            shutil.copytree(os.path.join(wdir, "io"), os.path.join(stage, "workers", w, "io"))
            sp = os.path.join(wdir, "io", "self.json")
            for inc in (json.load(open(sp, encoding="utf-8")).get("include", []) if os.path.exists(sp) else []):
                d = os.path.join(stage, "workers", w, inc)
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copy2(os.path.join(wdir, inc), d)
            common = os.path.join(stage, "workers", "_template", "io", "common.json")
            os.makedirs(os.path.dirname(common), exist_ok=True)
            shutil.copy2(os.path.join(ROOT, "workers", "_template", "io", "common.json"), common)
    # 시스템 모니터(기본 탑재): C++ 확정 실행 파일(monitor/bin/sysmon, 위 PLATFORM_BINS로 들어감). 개발 빌드 후보는 뺀다
    if product.get("system_monitor", True):
        mon = os.path.join(stage, "monitor")
        os.makedirs(mon, exist_ok=True)
        t = json.load(open(os.path.join(ROOT, "monitor", "worker-types.json"), encoding="utf-8"))
        for v in t.values():
            v["exec"] = ["@WORKSPACE_ROOT@/monitor/bin/sysmon"]
            v.pop("exec_candidates", None)
        json.dump(t, open(os.path.join(mon, "worker-types.json"), "w", encoding="utf-8"), indent=2, ensure_ascii=False)
        for f in ("config.json", "presets"):
            cp(os.path.join("monitor", f))
    # 매니저(기본 탑재, docs/managers.md): 늘 넣는다. C++ 확정 실행 파일(위 PLATFORM_BINS), 개발 빌드 후보는 뺀다
    for m in sorted(os.listdir(os.path.join(ROOT, "managers"))):
        src = os.path.join(ROOT, "managers", m, "worker-types.json")
        if not os.path.isfile(src):
            continue
        dst = os.path.join(stage, "managers", m)
        os.makedirs(dst, exist_ok=True)
        t = json.load(open(src, encoding="utf-8"))
        for v in t.values():
            v.pop("exec_candidates", None)
        json.dump(t, open(os.path.join(dst, "worker-types.json"), "w", encoding="utf-8"), indent=2, ensure_ascii=False)
        for f in ("config.json", "presets"):
            cp(os.path.join("managers", m, f))
    if product.get("ui"):
        # 기본 화면만: 셸·공통 부품 + 워커에 딸린 패널(worker_types: 시스템 모니터). 진단(로그·명령 이력)·설정(워크스페이스)
        # 패널은 파일째 뺀다(주소로도 못 엶). 워커 패널은 위에서 워커 폴더와 함께 들어갔다. 별도 제어 UI는 REST로 만든다
        cp("ui/web")
        pdir = os.path.join(stage, "ui", "web", "panels")
        for name in sorted(os.listdir(pdir)):
            p = os.path.join(pdir, name)
            mf = os.path.join(p, "panel.json")
            if os.path.isfile(mf) and "worker_types" in json.load(open(mf, encoding="utf-8")):
                continue
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    cp("install/product/install.sh", "install.sh")
    for name, val in (("PRODUCT_NAME", product["name"]), ("VERSION", version),
                      ("PRODUCT_UI", "1" if product.get("ui") else "0"),
                      ("PRODUCT_POWER_MODE", product.get("power_mode") or "keep"),
                      ("PRODUCT_CONFIG_WRITE", "1" if product.get("config_write") else "0")):
        open(os.path.join(stage, name), "w").write(val + "\n")


def compile_apps(stage, version, runner):
    """aarch64(이 보드 또는 ssh 보드)의 임시 폴더에서 Nuitka로 컴파일·심볼 제거하고 결과만 가져온다. 진행률 15~92"""
    remote = "/tmp/workspace-product-%s" % version
    work = tempfile.mkdtemp(prefix="wsprod-src-")
    try:
        os.makedirs(os.path.join(work, "src"), exist_ok=True)  # 컴파일할 앱이 없어도 묶음 모양은 같게
        for name, a in APPS.items():
            d = os.path.join(work, "src", a["src"])
            if not os.path.exists(d):
                shutil.copytree(os.path.join(ROOT, a["src"]), d,
                                ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "test_*.py", "*.md", "*.json", "presets"))
            with open(os.path.join(work, "src", "main_%s.py" % name), "w") as f:
                f.write("import sys\n%s\nsys.exit(main())\n" % a["entry"])
        script = r'''set -e
R=%(remote)s; DEPS=%(deps)s
mkdir -p "$DEPS"
if [ ! -x "$DEPS/bin/nuitka" ]; then
  echo "##PROGRESS 16 컴파일 도구 받는 중(처음 한 번)"
  python3 -m pip install -q --target "$DEPS" nuitka patchelf ordered-set
fi
export PYTHONPATH="$DEPS" PATH="$DEPS/bin:$PATH"
cd "$R/src"
''' % {"remote": remote, "deps": DEPS}
        n = len(APPS)
        for k, (name, a) in enumerate(APPS.items()):
            pp = "$R/src/%s" % a["src"] + (":$R/src/%s" % a["extra_path"] if a.get("extra_path") else "")
            inc = " ".join(["--include-package=%s" % a["pkg"]] if a["pkg"] else []) + "".join(
                " --include-module=%s" % m for m in a.get("include", []))
            script += 'echo "##PROGRESS %d 앱 컴파일 %d/%d · %s"\n' % (18 + k * 66 // n, k + 1, n, name)
            script += ('PYTHONPATH="%s:$DEPS" python3 -m nuitka --standalone --assume-yes-for-downloads --quiet --remove-output '
                       '--output-dir=$R/out --output-filename=%s %s main_%s.py\n'
                       'rm -rf $R/app/%s && mkdir -p $R/app && mv $R/out/main_%s.dist $R/app/%s\n') % (pp, name, inc, name, name, name, name)
        # 심볼 제거: 워커 실행 파일과 컴파일한 실행 파일에서 함수·파일 이름을 지운다(역분석을 어렵게). aarch64 strip
        bins = os.path.join(work, "bins")
        for w in os.listdir(os.path.join(stage, "workers")):
            b = os.path.join(stage, "workers", w, "workspace", "bin")
            for f in os.listdir(b) if os.path.isdir(b) else ():
                if f != "BUILD":
                    os.makedirs(os.path.join(bins, "workers", w, "workspace", "bin"), exist_ok=True)
                    shutil.copy2(os.path.join(b, f), os.path.join(bins, "workers", w, "workspace", "bin", f))
        script += 'echo "##PROGRESS 85 심볼 제거"\n'
        for d, names in PLATFORM_BINS.items():
            for n in names:
                if n != "BUILD":
                    os.makedirs(os.path.join(bins, d), exist_ok=True)
                    shutil.copy2(os.path.join(stage, d, n), os.path.join(bins, d, n))
        script += ('cd $R && for f in $(find bins -type f) %s; do strip --strip-unneeded "$f"; done\n'
                   % " ".join("app/%s/%s" % (a, a) for a in APPS))
        script += "cd $R && mkdir -p app && tar czf app.tgz app && tar czf bins.tgz -C bins .\n"
        tar = os.path.join(work, "src.tgz")
        with tarfile.open(tar, "w:gz") as t:
            t.add(os.path.join(work, "src"), arcname="src")
            t.add(bins, arcname="bins")
        progress(15, "컴파일 준비(%s)" % runner.where)
        runner.sh("rm -rf %s && mkdir -p %s" % (remote, remote))
        with open(tar, "rb") as f:
            runner.sh("tar xzf - --warning=no-timestamp -C %s" % remote, stdin=f)
        t0 = time.time()
        runner.sh("bash -s", input=script.encode())
        print("compiled on %s in %.0f s" % (runner.where, time.time() - t0), flush=True)
        progress(90, "결과 가져오기")
        for name in ("app.tgz", "bins.tgz"):
            with open(os.path.join(work, name), "wb") as f:
                runner.sh("cat %s/%s" % (remote, name), stdout=f)
        with tarfile.open(os.path.join(work, "app.tgz")) as t:
            t.extractall(stage)
        with tarfile.open(os.path.join(work, "bins.tgz")) as t:
            t.extractall(stage)  # 심볼을 지운 워커 실행 파일로 바꿔 놓음
    finally:
        shutil.rmtree(work, ignore_errors=True)
        try:
            runner.sh("rm -rf %s" % remote)
        except Exception:
            pass


def sums(stage):
    lines = []
    for dp, _, fs in os.walk(stage):
        for f in sorted(fs):
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, stage)
            if rel == "SHA256SUMS" or os.path.islink(p):
                continue
            h = hashlib.sha256(open(p, "rb").read()).hexdigest()
            lines.append("%s  %s" % (h, rel))
    open(os.path.join(stage, "SHA256SUMS"), "w").write("\n".join(sorted(lines, key=lambda l: l[66:])) + "\n")


def build(a):
    product = load_product(a.profile)
    if a.power_mode:  # 화면 빌드 모달에서 고른 값이 product.json보다 우선
        product["power_mode"] = a.power_mode
    if not re.match(r"^[A-Za-z0-9_]{1,32}$", product.get("power_mode") or "keep"):
        raise BuildError("bad power_mode: %r" % product.get("power_mode"))
    benv = board_env()
    local = is_local(a.local, benv)
    progress(1, "점검")
    items, info = check(product, local, a.allow_dirty, a.out)
    bad = [i for i in items if not i["ok"]]
    if bad:
        raise BuildError("; ".join("%s: %s" % (i["label"], i["detail"]) for i in bad))
    runner = Runner(local, benv) if not a.skip_compile else None
    commit = git("rev-parse", "--short", "HEAD")
    version = time.strftime("%Y%m%d-%H%M") + "-" + commit
    name = "%s-%s" % (product["name"], version)
    stage = os.path.join(a.out, name)
    out = os.path.join(a.out, name + ".tar.gz")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    try:
        progress(8, "묶음 준비")
        stage_tree(product, info, stage, version)
        if runner:
            compile_apps(stage, version, runner)
        progress(93, "목록·검사 합계")
        manifest = {"name": product["name"], "version": version, "commit": commit, "profile": a.profile,
                    "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "ui": bool(product.get("ui")),
                    "config_write": bool(product.get("config_write")), "power_mode": product.get("power_mode") or "keep",
                    "workers": info,
                    "compiled": not a.skip_compile, "stripped": not a.skip_compile,
                    "note": "worker binaries are stripped; workspace/bin/BUILD sha256 refers to the unstripped release"}
        json.dump(manifest, open(os.path.join(stage, "MANIFEST.json"), "w", encoding="utf-8"), indent=2, ensure_ascii=False)
        sums(stage)
        progress(96, "압축")
        with tarfile.open(out + ".part", "w:gz") as t:
            t.add(stage, arcname=name)
        os.replace(out + ".part", out)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
        if os.path.exists(out + ".part"):
            os.remove(out + ".part")
    if a.replace:  # 마지막 묶음 하나만 둔다(화면 빌드: 90 MB씩 쌓이지 않게)
        for old in glob.glob(os.path.join(a.out, "*.tar.gz")):
            if os.path.abspath(old) != os.path.abspath(out):
                os.remove(old)
    sha = hashlib.sha256(open(out, "rb").read()).hexdigest()
    result = {"file": os.path.basename(out), "path": out, "mb": round(os.path.getsize(out) / 1e6, 1), "sha256": sha,
              "version": version, "profile": a.profile, "stripped": not a.skip_compile,
              "power_mode": product.get("power_mode") or "keep"}
    progress(100, "완료")
    print("##RESULT " + json.dumps(result, ensure_ascii=False), flush=True)
    print("product bundle: %s (%.1f MB)" % (out, os.path.getsize(out) / 1e6))
    print("install on the device: tar xzf %s.tar.gz && sudo bash %s/install.sh --owner <user>" % (name, name))


def main():
    ap = argparse.ArgumentParser(description="build a product bundle from the workspace")
    ap.add_argument("profile", nargs="?")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--local", action="store_true", help="compile here (this machine is the aarch64 board)")
    ap.add_argument("--replace", action="store_true", help="after success, remove other bundles in --out")
    ap.add_argument("--power-mode", help="power mode the device install sets (e.g. MAXN_SUPER, 25W, keep); default product.json power_mode")
    ap.add_argument("--check", action="store_true", help="only run the pre-build checks")
    ap.add_argument("--json", action="store_true", help="with --check: print JSON")
    ap.add_argument("--profiles", action="store_true", help="list profiles that have product.json (JSON)")
    ap.add_argument("--allow-dirty", action="store_true", help="uncommitted changes / changed worker sources (test only)")
    ap.add_argument("--skip-compile", action="store_true", help="no Python compile (test only: bundle needs app/)")
    a = ap.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))  # 취소(화면): finally에서 임시 폴더를 지운다
    if a.profiles:
        print(json.dumps(profiles(), ensure_ascii=False))
        return 0
    if not a.profile:
        ap.error("profile is required")
    try:
        if a.check:
            items, _ = check(load_product(a.profile), is_local(a.local, board_env()), a.allow_dirty, a.out)
            ok = all(i["ok"] for i in items)
            if a.json:
                print(json.dumps({"ok": ok, "profile": a.profile, "items": items}, ensure_ascii=False))
            else:
                for i in items:
                    print("%-4s %-34s %s" % ("OK" if i["ok"] else "FAIL", i["label"], i["detail"]))
            return 0 if ok else 1
        build(a)
        return 0
    except (BuildError, subprocess.CalledProcessError, OSError) as e:
        print("##ERROR %s" % e, flush=True)
        print("build-product: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
