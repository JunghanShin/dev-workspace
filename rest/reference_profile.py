"""이 보드의 워크스페이스·프로필 정보(읽기 전용, GET /system/workspace).

워크스페이스 설정은 파일로만 바꾼다(design.md 6.10). 화면은 보기만 한다:
  - 적용한 프로필: <데이터>/config/profile.json (install_workspace.sh가 프로필을 넣을 때 기록)
  - 설정 파일(워크스페이스 기본 파일 + 적용한 프로필의 파일): 보드 내용, 출처(프로필 그대로 / 기본값 / 보드에서 바뀜)
  - workspace.env 키마다 값, 기본값, 설명(기본 파일 install/config/workspace.env의 주석)
비밀값(이름에 PASS·TOKEN·SECRET·API_KEY가 들어간 값)은 가린다. 파일 경로는 고정이다(요청이 경로를 정하지 않음).
"""
import glob
import json
import os
import re
import subprocess
import time

# 워크스페이스 기본 설정 파일(install/config/의 파일 중 프로필로 바꾸는 것). 프로필은 이 밖의 파일도 가질 수 있다
# (워커가 쓰는 설정 등, install_workspace.sh profile_files와 같은 규칙)
NOT_PROFILE = ("worker-types.json", "product.json")


def profile_files(folder):
    """폴더 맨 위의 설정 파일 이름(README·*.md·product.json·숨김 제외)"""
    if not folder or not os.path.isdir(folder):
        return []
    return sorted(f for f in os.listdir(folder)
                  if os.path.isfile(os.path.join(folder, f)) and not f.startswith((".", "README"))
                  and not f.endswith(".md") and f not in NOT_PROFILE)
MAX_TEXT = 64 * 1024
MASK = "••••"
_SECRET_NAME = re.compile(r"(?i)(pass(word|wd)?|token|secret|api_?key|private_?key)")
_SECRET_LINE = re.compile(r"(?i)^(\s*[\w.\-\"]*(pass(word|wd)?|token|secret|api_?key|private_?key)[\w.\-\"]*\s*[:=]\s*)(.+)$")
_ENV_LINE = re.compile(r"^\s*(#\s*)?([A-Z][A-Z0-9_]*)=(.*)$")


def _read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read(MAX_TEXT + 1)
    except OSError:
        return None


def _mask_text(text):
    return "\n".join(_SECRET_LINE.sub(lambda m: m.group(1) + MASK, line) for line in text.split("\n"))


def _git(root, *args):
    # --no-optional-locks: status가 index.lock을 잡지 않게(시간 초과로 끊겨도 git pull을 막지 않음)
    try:
        p = subprocess.run(["git", "--no-optional-locks", "-c", "safe.directory=" + root, "-C", root, *args],
                           capture_output=True, text=True, timeout=3)
        return p.stdout.strip() if p.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


GIT_CACHE_S = 30
_git_cache = {"t": 0.0, "v": None}


def _git_info(root):
    """워크스페이스 커밋·브랜치·고친 파일 여부. git status가 느릴 수 있어 GIT_CACHE_S초 동안 재사용"""
    now = time.monotonic()
    if _git_cache["v"] is None or now - _git_cache["t"] > GIT_CACHE_S:
        _git_cache["v"] = {"root": root, "commit": _git(root, "rev-parse", "--short", "HEAD"),
                           "branch": _git(root, "rev-parse", "--abbrev-ref", "HEAD"),
                           "dirty": None}
        st = _git(root, "status", "--porcelain", "--untracked-files=no")
        _git_cache["v"]["dirty"] = None if st is None else bool(st)  # None = 확인 못 함(시간 초과 등)
        _git_cache["t"] = now
    return _git_cache["v"]


def _env_doc(text):
    """기본 workspace.env → [{key, default, example, desc, group}]. 키 바로 위 주석 줄들이 설명, "## "로 시작하는 줄은 묶음 제목"""
    out, desc, group = [], [], ""
    for line in (text or "").splitlines():
        m = _ENV_LINE.match(line)
        if m:
            out.append({"key": m.group(2), "default": None if m.group(1) else m.group(3),
                        "example": m.group(3) if m.group(1) else None, "desc": " ".join(desc), "group": group})
            desc = []
        elif line.startswith("##"):  # 묶음 제목: 아래 키들에 이어서 붙는다
            group, desc = line.lstrip("#").strip(), []
        elif line.startswith("#"):
            desc.append(line.lstrip("#").strip())
        else:
            desc = []
    return out


def _env_values(text):
    vals = {}
    for line in (text or "").splitlines():
        m = _ENV_LINE.match(line)
        if m and not m.group(1):
            vals[m.group(2)] = m.group(3)
    return vals


def info(workspace, data_dir):
    cfg = os.path.join(data_dir, "config")
    defaults_dir = os.path.join(workspace, "install", "config")

    profile = None
    raw = _read(os.path.join(cfg, "profile.json"))
    if raw:
        try:
            profile = json.loads(raw)
        except ValueError:
            profile = {"error": "profile.json을 읽지 못함"}
    src_dir = None
    if profile and profile.get("source"):
        p = profile["source"]
        src_dir = p if os.path.isabs(p) else os.path.join(workspace, p)
        if not os.path.isdir(src_dir):
            src_dir = None
    if profile is None:
        # 기록 전에 설치한 보드: 보드 파일과 가장 많이 같은 프로필을 추정(비교에만 쓴다)
        best, best_n = None, 0
        for d in sorted(glob.glob(os.path.join(workspace, "install", "profiles", "*", ""))):
            n = sum(1 for f in profile_files(d) if _read(os.path.join(cfg, f)) is not None
                    and _read(os.path.join(d, f)) == _read(os.path.join(cfg, f)))
            if n > best_n:
                best, best_n = d, n
        if best:
            src_dir = best.rstrip("/")
            profile = {"name": os.path.basename(src_dir), "source": os.path.relpath(src_dir, workspace),
                       "guessed": True, "matching_files": best_n}

    files = []
    names = profile_files(defaults_dir)
    names += [f for f in profile_files(src_dir) if f not in names]
    for name in names:
        board = _read(os.path.join(cfg, name))
        prof = _read(os.path.join(src_dir, name)) if src_dir else None
        default = _read(os.path.join(defaults_dir, name))
        if board is None:
            origin = "missing"
        elif prof is not None:
            origin = "profile" if board == prof else "changed"
        elif default is not None and board == default:
            origin = "default"
        else:
            origin = "changed" if (src_dir or default is not None) else "unknown"
        files.append({
            "name": name,
            "path": os.path.join(cfg, name),
            "exists": board is not None,
            "in_profile": prof is not None,
            "origin": origin,
            "truncated": board is not None and len(board) > MAX_TEXT,
            "text": _mask_text(board[:MAX_TEXT]) if board is not None else None,
        })

    env_board = _env_values(_read(os.path.join(cfg, "workspace.env")))
    env_prof = _env_values(_read(os.path.join(src_dir, "workspace.env"))) if src_dir else {}
    env = []
    seen = set()
    for d in _env_doc(_read(os.path.join(defaults_dir, "workspace.env"))):
        seen.add(d["key"])
        env.append(d)
    for key in env_board:
        if key not in seen:
            env.append({"key": key, "default": None, "example": None, "desc": ""})
    for e in env:
        k = e["key"]
        secret = bool(_SECRET_NAME.search(k))
        e["value"] = (MASK if secret else env_board[k]) if k in env_board else None
        e["in_profile"] = k in env_prof
        if secret and e.get("default"):
            e["default"] = MASK

    return {
        "workspace": _git_info(workspace),
        "data_dir": data_dir,
        "profile": profile,
        "files": files,
        "env": env,
    }
