"""워커 설정 스키마 검사 (design.md 6.10).

워커 type 선언의 "config_schema"는 화면이 설정 폼을 그리는 데 쓰고, Manager는 저장하기 전에 같은
스키마로 값을 검사한다. 스키마에 없는 키(내부 값: command_socket 등)는 검사하지 않고 그대로 둔다.

  {"groups": [{"title": "...", "hint": "...", "advanced": false, "section": "<모듈>", "fields": [칸, ...]}]}

  section  이 묶음의 칸이 config["<모듈>"] 안에 있다(워커 모듈 구조: 모듈마다 설정 칸, design.md 6.6.3).
           없으면 config 맨 위(호스트 키 등). 오류 경로는 "<모듈>.<키>"

칸: {"key", "label", "type", ...}
  type      string | int | number | bool | enum | list | map
  default   기본값(비워 두면 워커가 쓰는 값). 화면의 "기본값으로"와 자리 표시에 쓴다
  required  true면 비울 수 없음(list 항목 안에서 주로 쓴다)
  min, max  int·number·map 값 범위
  options   enum 선택지: [값, ...] 또는 [[값, 이름], ...]
  format    string 형식: ipv4 | host_port | stream_url | worker_id(다른 워커의 id: 워커 간 통신 상대, design.md 6.6.7)
  pattern   string 정규식
  list:     item_key(항목마다 달라야 하는 키), fields(항목의 칸)
  map:      key_pattern, value_type(int | number | string), min, max
  꾸밈:     label, unit, hint, placeholder, empty_label(비웠을 때 이름), advanced(접어 두는 칸) — 검사에는 쓰지 않는다
"""
import ipaddress
import re

TYPES = ("string", "int", "number", "bool", "enum", "list", "map")
_HOST_RE = re.compile(r"^[A-Za-z0-9]([A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$")
_URL_RE = re.compile(r"^(rtsp|rtsps|srt|udp|rtmp)://[^/\s:]+(:\d{1,5})?(/\S*)?$")
_WORKER_ID_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")


def fields_of(schema):
    """스키마 → 칸 목록(묶음의 section과 상관없이 모두)"""
    out = []
    for g in (schema or {}).get("groups") or ():
        out.extend(g.get("fields") or ())
    return out


def sections_of(schema):
    """스키마 → [(section 또는 None, 칸 목록)]. 같은 section의 묶음은 하나로 합친다"""
    out = {}
    for g in (schema or {}).get("groups") or ():
        out.setdefault(g.get("section"), []).extend(g.get("fields") or ())
    return list(out.items())


def check_schema(schema):
    """스키마 자체의 형식 오류 목록(워커 개발자용). 비어 있으면 정상"""
    errs = []

    def walk(fields, where):
        seen = set()
        for f in fields:
            k = f.get("key")
            if not isinstance(k, str) or not k:
                errs.append("%s: field without key" % where)
                continue
            if k in seen:
                errs.append("%s.%s: duplicate key" % (where, k))
            seen.add(k)
            t = f.get("type")
            if t not in TYPES:
                errs.append("%s.%s: unknown type %r" % (where, k, t))
            if t == "enum" and not f.get("options"):
                errs.append("%s.%s: enum without options" % (where, k))
            if t == "list":
                walk(f.get("fields") or (), "%s.%s[]" % (where, k))
            if "default" in f:
                e = _check_value(f, f["default"], k)
                errs.extend("%s.%s default: %s" % (where, k, m) for m in e.values())

    if not isinstance(schema, dict) or not isinstance(schema.get("groups"), list):
        return ["config_schema must be {\"groups\": [...]}"]
    for section, fields in sections_of(schema):
        if section is not None and (not isinstance(section, str) or not re.match(r"^[a-z][a-z0-9_]*$", section)):
            errs.append("bad section name %r" % section)
            continue
        walk(fields, "config" if section is None else "config." + section)
    return errs


def _options(f):
    return [o[0] if isinstance(o, list) else o for o in f.get("options") or ()]


def _same(a, b):
    """enum 비교: true와 1을 같은 값으로 보지 않는다"""
    return type(a) is type(b) and a == b or \
        isinstance(a, (int, float)) and isinstance(b, (int, float)) and \
        not isinstance(a, bool) and not isinstance(b, bool) and a == b


def _num(f, v, path, errs, integer):
    if isinstance(v, bool) or not isinstance(v, (int, float)) or (integer and not float(v).is_integer()):
        errs[path] = "정수여야 합니다" if integer else "숫자여야 합니다"
        return
    lo, hi = f.get("min"), f.get("max")
    if lo is not None and v < lo or hi is not None and v > hi:
        errs[path] = "%s ~ %s 사이여야 합니다" % (_fmt(lo), _fmt(hi)) if lo is not None and hi is not None \
            else ("%s 이상이어야 합니다" % _fmt(lo) if lo is not None else "%s 이하여야 합니다" % _fmt(hi))


def _fmt(x):
    return "%g" % x if isinstance(x, float) else str(x)


def _string(f, v, path, errs):
    if not isinstance(v, str):
        errs[path] = "글자여야 합니다"
        return
    fmt = f.get("format")
    if fmt == "ipv4":
        try:
            ipaddress.IPv4Address(v)
        except ValueError:
            errs[path] = "IPv4 주소여야 합니다(예: 192.168.1.10)"
            return
    elif fmt == "host_port":
        host, _, port = v.rpartition(":")
        if not host or not port.isdigit() or not 1 <= int(port) <= 65535 or not _HOST_RE.match(host):
            errs[path] = "호스트:포트 형식이어야 합니다(예: 127.0.0.1:9000)"
            return
    elif fmt == "stream_url":
        if not _URL_RE.match(v):
            errs[path] = "rtsp://호스트:포트/경로 같은 주소여야 합니다"
            return
    elif fmt == "worker_id":
        if v and not _WORKER_ID_RE.match(v):  # 빈 값 = 상대 없음(꺼짐)
            errs[path] = "워커 id여야 합니다(영문 소문자·숫자·-·_)"
            return
    pat = f.get("pattern")
    if pat and not re.search(pat, v):
        errs[path] = "형식이 맞지 않습니다"


def _check_value(f, v, path):
    """칸 하나의 값 → {경로: 메시지}"""
    errs = {}
    t = f.get("type")
    if t == "string":
        _string(f, v, path, errs)
    elif t == "int":
        _num(f, v, path, errs, True)
    elif t == "number":
        _num(f, v, path, errs, False)
    elif t == "bool":
        if not isinstance(v, bool):
            errs[path] = "켜기·끄기(true/false)여야 합니다"
    elif t == "enum":
        if not any(_same(v, o) for o in _options(f)):
            errs[path] = "선택지 중 하나여야 합니다: %s" % ", ".join(str(o) for o in _options(f))
    elif t == "list":
        if not isinstance(v, list):
            errs[path] = "목록이어야 합니다"
            return errs
        item_key, seen = f.get("item_key"), set()
        for i, item in enumerate(v):
            p = "%s[%d]" % (path, i)
            if not isinstance(item, dict):
                errs[p] = "항목은 객체여야 합니다"
                continue
            errs.update(check_object(f.get("fields") or (), item, p + "."))
            if item_key and isinstance(item.get(item_key), str):
                if item[item_key] in seen:
                    errs["%s.%s" % (p, item_key)] = "같은 값이 이미 있습니다: %s" % item[item_key]
                seen.add(item[item_key])
    elif t == "map":
        if not isinstance(v, dict):
            errs[path] = "키·값 목록이어야 합니다"
            return errs
        vt = f.get("value_type", "string")
        kp = f.get("key_pattern")
        for k, x in v.items():
            p = "%s.%s" % (path, k)
            if kp and not re.search(kp, k):
                errs[p] = "이름 형식이 맞지 않습니다: %s" % k
            elif vt in ("int", "number"):
                _num(f, x, p, errs, vt == "int")
            elif not isinstance(x, str):
                errs[p] = "글자여야 합니다"
    return errs


def check_object(fields, obj, prefix=""):
    """칸 목록으로 객체 하나를 검사 → {경로: 메시지}. 없는 칸은 required일 때만 오류"""
    errs = {}
    for f in fields:
        k = f.get("key")
        path = prefix + k
        if k not in obj or obj[k] is None:
            if f.get("required"):
                errs[path] = "비울 수 없습니다"
            continue
        errs.update(_check_value(f, obj[k], path))
    return errs


def worker_refs(schema, config):
    """설정에서 다른 워커를 가리키는 값(칸 형식 worker_id) → {경로: 워커 id}. 워커 간 연결 관계(화면·시스템 시험)"""
    out = {}
    if not schema or not isinstance(config, dict):
        return out

    def walk(fields, obj, prefix):
        if not isinstance(obj, dict):
            return
        for f in fields:
            k, v = f.get("key"), obj.get(f.get("key"))
            if v is None:
                continue
            if f.get("type") == "string" and f.get("format") == "worker_id" and isinstance(v, str) and v:
                out[prefix + k] = v
            elif f.get("type") == "list" and isinstance(v, list):
                for i, item in enumerate(v):
                    walk(f.get("fields") or (), item, "%s%s[%d]." % (prefix, k, i))

    for section, fields in sections_of(schema):
        walk(fields, config if section is None else config.get(section) or {}, "" if section is None else section + ".")
    return out


def check_config(schema, config):
    """워커 설정 전체 검사 → {경로: 메시지}. 스키마가 없으면 검사하지 않는다"""
    if not schema:
        return {}
    errs = {}
    for section, fields in sections_of(schema):
        if section is None:
            errs.update(check_object(fields, config))
            continue
        sub = config.get(section)
        if sub is None:
            sub = {}
        if not isinstance(sub, dict):
            errs[section] = "객체여야 합니다"
            continue
        errs.update(check_object(fields, sub, section + "."))
    return errs
