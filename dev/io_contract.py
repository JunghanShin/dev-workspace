"""입출력 정의(계약) 읽기·합치기·검사(워크스페이스 design.md 6.6.7.1~6.6.7.2). 표준 라이브러리만.

정의 세 가지를 합쳐 워커 하나의 계약을 만든다:
  공통 정의  workers/_template/io/common.json   (get_status 겉모양, 공통 명령, 호스트·단계 지표, 스트림 겉모양, 이벤트)
  워커 정의  workers/<w>/io/self.json + include  (모듈별 status, 지표, 진단 주제, 정비 명령)
  역할 정의  io.json provides → io/<역할>.json 또는 io/uses/<역할>.json

검사기는 정의에 쓰는 JSON Schema 부분만 구현한다: type, properties, required, additionalProperties(적지 않으면 false),
items, enum, minimum/maximum, exclusiveMinimum/exclusiveMaximum, $ref(#/definitions/<이름>). 덧붙임 칸 unit·description·optional은 무시.
"""
import json
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TEMPLATE = os.path.join(ROOT, "workers", "_template")
NOT_ROLES = {"self", "common"}  # io/ 안에서 역할 정의가 아닌 파일
HEARTBEAT_FIXED = {"WATCHDOG", "READY", "id", "progress", "config_version", "libworker", "sim"}


class ContractError(Exception):
    pass


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def pattern(name):
    """'stream_<송출>_fps' → 정규식. 자리(<…>)는 [a-z0-9_]+"""
    parts = re.split(r"<[^<>]+>", name)
    return re.compile("^" + "[a-z0-9_]+".join(re.escape(p) for p in parts) + "$")


class Named:
    """이름(자리 <…> 가능) → 값. 정확한 이름 먼저, 다음 자리 이름"""

    def __init__(self):
        self.exact, self.patterns = {}, []

    def add(self, name, value, where):
        if name in self.exact or any(n == name for n, _, _ in self.patterns):
            raise ContractError("%s: '%s'가 두 번 정의됨" % (where, name))
        if "<" in name:
            self.patterns.append((name, pattern(name), value))
        else:
            self.exact[name] = value

    def find(self, name):
        if name in self.exact:
            return name, self.exact[name]
        for n, rx, v in self.patterns:
            if rx.match(name):
                return n, v
        return None, None

    def items(self):
        return list(self.exact.items()) + [(n, v) for n, _, v in self.patterns]


class Contract:
    """워커 하나의 합친 계약"""

    def __init__(self, worker_dir):
        self.worker_dir = worker_dir
        self.common = load_json(os.path.join(TEMPLATE, "io", "common.json"))
        self.defs = dict(self.common.get("definitions", {}))
        self.status, self.metrics, self.commands, self.topics = Named(), Named(), Named(), Named()
        self.errors = set(self.common.get("errors", []))
        self._merge(self.common, "common.json", status=True, metrics=True)
        self.self_path = os.path.join(worker_dir, "io", "self.json")
        self.self = load_json(self.self_path) if os.path.exists(self.self_path) else None
        if self.self is not None:
            self._merge(self.self, "self.json", status=True, metrics=True)
            for inc in self.self.get("include", []):
                p = os.path.join(worker_dir, inc)
                if not os.path.exists(p):
                    raise ContractError("self.json include %s: 파일 없음" % inc)
                self._merge(load_json(p), inc, status=True, metrics=True)
        io = load_json(os.path.join(worker_dir, "io.json"))
        self.io = io
        for role in io.get("provides", []):
            p = os.path.join(worker_dir, "io", role + ".json")
            if not os.path.exists(p):
                p = os.path.join(worker_dir, "io", "uses", role + ".json")
            self._merge(load_json(p), os.path.relpath(p, worker_dir), status=False, metrics=False)

    def _merge(self, d, where, status, metrics):
        for k, v in d.get("definitions", {}).items():
            if k in self.defs and self.defs[k] != v:
                raise ContractError("%s: definitions.%s가 다른 정의와 다름" % (where, k))
            self.defs[k] = v
        if status:
            for k, v in d.get("status", {}).items():
                self.status.add(k, v, where + " status")
        if metrics:
            for k, v in d.get("metrics", {}).items():
                self.metrics.add(k, v, where + " metrics")
        for k, v in d.get("commands", {}).items():
            self.commands.add(k, v, where + " commands")
        for k, v in d.get("topics", {}).items():
            self.topics.add(k, v, where + " topics")


def _type_ok(v, t):
    if t == "integer":
        return isinstance(v, int) and not isinstance(v, bool)
    if t == "number":
        return isinstance(v, (int, float)) and not isinstance(v, bool)
    if t == "null":
        return v is None
    kind = {"object": dict, "array": list, "string": str, "boolean": bool}.get(t)
    return kind is not None and isinstance(v, kind)


def validate(v, s, defs, path, errs):
    """v를 스키마 s로 검사, 틀린 곳을 errs에 '경로: 이유'로 더한다"""
    if s is True or s is None:
        return
    if "$ref" in s:
        name = s["$ref"].split("/")[-1]
        if name not in defs:
            errs.append("%s: $ref %s 없음" % (path, s["$ref"]))
            return
        return validate(v, defs[name], defs, path, errs)
    t = s.get("type")
    if t is not None:
        types = t if isinstance(t, list) else [t]
        if not any(_type_ok(v, x) for x in types):
            errs.append("%s: 형식이 %s가 아님(%s)" % (path, "|".join(types), type(v).__name__))
            return
    if "enum" in s and v not in s["enum"]:
        errs.append("%s: %r가 %s 중 하나가 아님" % (path, v, s["enum"]))
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        if "minimum" in s and v < s["minimum"]:
            errs.append("%s: %r < 최소 %r" % (path, v, s["minimum"]))
        if "maximum" in s and v > s["maximum"]:
            errs.append("%s: %r > 최대 %r" % (path, v, s["maximum"]))
        if "exclusiveMinimum" in s and v <= s["exclusiveMinimum"]:
            errs.append("%s: %r <= %r" % (path, v, s["exclusiveMinimum"]))
        if "exclusiveMaximum" in s and v >= s["exclusiveMaximum"]:
            errs.append("%s: %r >= %r" % (path, v, s["exclusiveMaximum"]))
    if isinstance(v, dict):
        props = s.get("properties", {})
        for k in s.get("required", []):
            if k not in v:
                errs.append("%s.%s: 필수 칸 없음" % (path, k))
        extra = s.get("additionalProperties", False)
        for k, sub in v.items():
            if k in props:
                validate(sub, props[k], defs, "%s.%s" % (path, k), errs)
            elif extra is False:
                errs.append("%s.%s: 정의에 없음" % (path, k))
            elif isinstance(extra, dict):
                validate(sub, extra, defs, "%s.%s" % (path, k), errs)
    if isinstance(v, list) and "items" in s:
        for i, x in enumerate(v):
            validate(x, s["items"], defs, "%s[%d]" % (path, i), errs)


def parse_heartbeat(text):
    """'WATCHDOG=1 id=a progress=3 config_version=1 source_seq=4 …' → 지표 dict(고정 칸 빼고). 값은 int/float/str"""
    out = {}
    for tok in text.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        if k in HEARTBEAT_FIXED:
            continue
        if re.fullmatch(r"-?\d+", v):
            out[k] = int(v)
        else:
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
    return out
