"""보드에서 워크스페이스 화면 제공 (docs/local-ui-media.md).

화면(ui/web)은 이 REST가 제공한다. 보드와 같은 망에 붙은 PC는 브라우저만 열면 된다
(http://<Jetson>:8080).

  GET  /, /<파일>              화면 파일(ui/web)
  *    /api/board/<경로>        이 REST 경로 그대로(접두어만 뗌)
  GET  /api/remote/<이름>       보드 읽기(reads.py) → {"ok", "lines"}
  GET  /api/panels             패널 목록: ui/web/panels/*/panel.json(공용 패널)과
                               workers/<워커>/workspace/panel/panel.json(워커가 가진 패널, id는 panel.json "id")
  GET  /api/ui                 화면 설정: 갱신 주기, 영상 주소(이 보드의 MediaMTX WebRTC)

"""
import json
import logging
import mimetypes
import os
import time
from urllib.parse import urlsplit

from . import reads

log = logging.getLogger("workspace.rest.ui")

WEB = os.path.abspath(os.environ.get("WORKSPACE_UI_DIR") or os.path.join(reads.WORKSPACE, "ui", "web"))
# 영상 주소 틀. {path}는 화면이 채우고, {host}는 여기서 브라우저가 접속한 이 보드 주소로 채운다
MEDIA_URL = os.environ.get("WORKSPACE_UI_MEDIA_URL") or "http://{host}:8889/{path}"
REFRESH_MS = int(os.environ.get("WORKSPACE_UI_REFRESH_MS") or 1000)
# 화면 제목(왼쪽 위, 브라우저 탭). 프로젝트 이름은 프로필의 workspace.env에 둔다
TITLE = os.environ.get("WORKSPACE_UI_TITLE") or "Workspace"


def available():
    return os.path.isfile(os.path.join(WEB, "shell.html"))


WORKERS = os.path.join(reads.WORKSPACE, "workers")


def worker_panel_dirs():
    """{패널 id: 폴더} — 워커 폴더 안의 패널(workers/<워커>/workspace/panel/). _로 시작하는 폴더(틀)는 뺀다"""
    out = {}
    for name in sorted(os.listdir(WORKERS)) if os.path.isdir(WORKERS) else []:
        d = os.path.join(WORKERS, name, "workspace", "panel")
        manifest = os.path.join(d, "panel.json")
        if name.startswith("_") or not os.path.isfile(manifest):
            continue
        try:
            with open(manifest, encoding="utf-8") as f:
                pid = json.load(f).get("id") or name
        except (OSError, ValueError) as e:
            log.error("worker panel %s skipped: %s", name, e)
            continue
        out[pid] = d
    return out


def static_file(rel):
    """(데이터, Content-Type) 또는 None. ui/web와 워커 패널 폴더 밖은 주지 않는다"""
    rel = rel.lstrip("/")
    base = WEB
    parts = rel.split("/")
    # /panels/<id>/...: 워커가 가진 패널이면 그 워커 폴더에서
    if len(parts) >= 3 and parts[0] == "panels" and not os.path.isdir(os.path.join(WEB, "panels", parts[1])):
        wp = worker_panel_dirs().get(parts[1])
        if wp:
            base, rel = wp, "/".join(parts[2:])
    path = os.path.normpath(os.path.join(base, rel))
    if not path.startswith(base + os.sep) or not os.path.isfile(path):
        return None
    ctype = mimetypes.guess_type(path)[0] or "application/octet-stream"
    if path.endswith(".js"):
        ctype = "text/javascript"
    if ctype.startswith("text") or ctype in ("application/json", "image/svg+xml"):
        ctype += "; charset=utf-8"
    with open(path, "rb") as f:
        return f.read(), ctype


def panels():
    out = []
    root = os.path.join(WEB, "panels")
    for name in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        manifest = os.path.join(root, name, "panel.json")
        if not os.path.isfile(manifest):
            continue
        try:
            with open(manifest, encoding="utf-8") as f:
                p = json.load(f)
        except (OSError, ValueError) as e:
            log.error("panel %s skipped: %s", name, e)
            continue
        p["id"] = name
        out.append(p)
    # 워커가 가진 패널. 공용 패널과 id가 겹치면 공용 패널을 쓴다
    have = {p["id"] for p in out}
    for pid, d in worker_panel_dirs().items():
        if pid in have:
            log.error("worker panel %s skipped: id already used by ui/web/panels", pid)
            continue
        try:
            with open(os.path.join(d, "panel.json"), encoding="utf-8") as f:
                p = json.load(f)
        except (OSError, ValueError):
            continue
        p["id"] = pid
        out.append(p)
    out.sort(key=lambda p: (p.get("order", 100), p["id"]))
    return out


def request_host(host_header):
    """Host 헤더 → 브라우저가 이 보드에 접속한 주소(포트 없이). IPv6는 [..]"""
    host = urlsplit("//" + (host_header or "")).hostname or "127.0.0.1"
    return "[%s]" % host if ":" in host else host


def ui_info(host_header, data_dir, product=False):
    return {
        "title": TITLE,
        "product": bool(product),  # 화면이 설정·워커 시작/정지 부품을 그리지 않는다(kit.js setProduct)
        "refresh_ms": REFRESH_MS,
        "media": {"url": MEDIA_URL.replace("{host}", request_host(host_header))},
        "board": {"data_dir": data_dir},
        "rest_last_ok": time.time(),
        "rest_last_error": None,
    }


# ---- 보드 읽기: 화면의 이름 → reads.py 경로 ------------------------------------------------

def _read_path(name, params):
    wid = params.get("id", "")
    if name == "worker_log":
        return "/logs/workers/%s" % wid, {"lines": params.get("lines", 200)}
    if name == "service_log":
        return "/logs/service", {"lines": params.get("lines", 200)}
    if name == "commands":
        return "/records/commands", {"lines": params.get("lines", 200)}
    if name == "processes":
        return "/system/processes", {"lines": params.get("lines", 12)}
    if name == "power_modes":
        return "/system/power_modes", {}
    if name == "metrics_history":
        return "/history/%s" % wid, {k: params[k] for k in ("minutes", "points", "keys", "csv") if k in params}
    raise KeyError(name)


def _lines(name, obj):
    """reads.py 응답 → 화면이 쓰는 줄 목록"""
    if name == "processes":
        return ["%d %s %.1f %.1f %d" % (p["pid"], p["name"].replace(" ", "_"), p["cpu"], p["mem"], p["rss_kb"])
                for p in obj.get("processes", [])]
    if name == "power_modes":
        return ["< POWER_MODEL ID=%s NAME=%s >" % (k, v)
                for k, v in sorted(obj.get("modes", {}).items(), key=lambda kv: int(kv[0]))]
    return obj.get("lines", [])


def remote(name, params, data_dir):
    """(상태 코드, 응답). 모르는 이름은 KeyError, 잘못된 값은 reads.BadRequest"""
    path, query = _read_path(name, params)
    for pattern, read_name in reads.ROUTES:
        match = pattern.match(path)
        if match:
            break
    else:
        raise reads.BadRequest("bad worker id")
    status, obj = reads.handle(read_name, match, {k: str(v) for k, v in query.items()}, data_dir)
    if status != 200:
        err = obj.get("error") or {}
        return 200, {"ok": False, "error": err.get("message") or "read %s: %d" % (name, status)}
    return 200, {"ok": True, "lines": _lines(name, obj)}
