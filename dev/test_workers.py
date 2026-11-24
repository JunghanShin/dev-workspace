"""워커 폴더 구조 검사(표준 라이브러리만, 워크스페이스 design.md 3장 "단계"와 6.6). 템플릿과 모든 워커를 본다:
  - 공용 라이브러리 사본(lib/lib*)이 틀(workers/_template)의 원본과 같은지(틀로 만든 플랫폼 블록 monitor/ 포함)
  - 폴더 구성: 워커(CMake·README·design·main·workspace/), 모듈(core/include·include·src·test·README·schema)
  - 층 규칙: 모듈 코어(① 유닛)가 libworker를 쓰지 않는지(쓰면 실패)
  - 패널 id 겹침, Python 도우미 사본
  - 입출력 정의(design.md 6.6.7.1~6.6.7.2): io.json·io/<역할>.json 형식, 가리키는 파일, uses가 쓰는 항목, 사본(io/uses/)이 원본과 같은지·호환되는지,
    공통 정의(틀 lib/libworker/io/common.json)·워커 정의(io/self.json + include 조각) 형식, 지표 이름 규칙, check(출력 대조 방법)
    (실제 출력 대조는 dev/test_io_output.py)
  - 미준수 목록(실패가 아니라 알림): 유닛 시험이 없는 모듈, ICD(설계서 3장)가 없는 워커, 입출력 정의가 없는 워커 → 워커 고도화 때 하나씩 없앤다

    python3 -m unittest dev/test_workers.py -v      (워크스페이스 폴더에서)

사본이 원본과 다르면: 원본을 고친 것이면 그 워커에 덮어쓰고 그 워커를 시험한다. 워커 쪽만 고친 것이면 원본(틀)에 먼저 반영한다.
"""
import filecmp
import glob
import json
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import io_contract as ic  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TEMPLATE = os.path.join(ROOT, "workers", "_template")
# 공용 라이브러리 = 틀에 있는 lib/lib*(원본). 워커는 쓰는 것만 사본으로 둔다. 틀의 예제 모듈(source 등)은 출발점이라 비교하지 않는다
COPIED_LIBS = tuple(sorted(os.path.basename(d) for d in glob.glob(os.path.join(TEMPLATE, "lib", "lib*")) if os.path.isdir(d)))
SKIP_DIRS = {"build", ".git", "__pycache__"}
# 워커 틀로 만든 플랫폼 블록(워커 폴더가 아님): 시스템 모니터 sysmon(C++), 매니저(managers/*, docs/managers.md).
# 사본 라이브러리·코어 층 규칙만 같이 본다
PLATFORM_WORKERS = (os.path.join(ROOT, "monitor"),) + tuple(
    sorted(d for d in glob.glob(os.path.join(ROOT, "managers", "*")) if os.path.isfile(os.path.join(d, "CMakeLists.txt"))))


def workers(with_template=False):
    return sorted(d for d in glob.glob(os.path.join(ROOT, "workers", "*"))
                  if os.path.isdir(d) and (with_template or not os.path.basename(d).startswith("_")))


def modules(w):
    """워커의 모듈 폴더(lib/<모듈>/schema.json이 있는 것)"""
    return sorted(d for d in glob.glob(os.path.join(w, "lib", "*")) if os.path.exists(os.path.join(d, "schema.json")))


def sources(top):
    for dirpath, dirnames, filenames in os.walk(top):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and not d.startswith("build")]
        for f in filenames:
            if f.endswith((".cpp", ".hpp", ".h", ".cc")):
                yield os.path.join(dirpath, f)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def files(top):
    out = set()
    for dirpath, dirnames, filenames in os.walk(top):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and not d.startswith("build")]
        for f in filenames:
            out.add(os.path.relpath(os.path.join(dirpath, f), top))
    return out


class LibCopies(unittest.TestCase):
    def test_copies_match_template(self):
        for w in workers() + list(PLATFORM_WORKERS):
            for lib in COPIED_LIBS:
                copy = os.path.join(w, "lib", lib)
                if not os.path.isdir(copy):
                    continue  # 그 라이브러리를 쓰지 않는 워커
                orig = os.path.join(TEMPLATE, "lib", lib)
                with self.subTest(worker=os.path.basename(w), lib=lib):
                    a, b = files(orig), files(copy)
                    self.assertEqual(sorted(a - b), [], "사본에 없는 파일")
                    self.assertEqual(sorted(b - a), [], "원본에 없는 파일")
                    _, diff, err = filecmp.cmpfiles(orig, copy, sorted(a), shallow=False)
                    self.assertEqual(diff + err, [], "내용이 다른 파일")


class Layout(unittest.TestCase):
    def test_worker_folders_follow_template(self):
        for w in workers(with_template=True):
            name = os.path.basename(w)
            with self.subTest(worker=name):
                need = ("CMakeLists.txt", "README.md", "src/main.cpp", "lib/libworker")
                need += ("new/design.md",) if name == "_template" else ("design.md",)
                for rel in need:
                    self.assertTrue(os.path.exists(os.path.join(w, rel)), rel)
                mods = [d for d in glob.glob(os.path.join(w, "lib", "*"))
                        if os.path.exists(os.path.join(d, "schema.json"))]
                self.assertTrue(mods, "lib/<모듈>/schema.json이 있는 모듈이 하나 이상")
                for m in mods:
                    rel = os.path.relpath(m, w)
                    for part in ("CMakeLists.txt", "README.md", "core/include", "include", "src", "test"):  # core/src는 헤더만인 유닛이면 없어도 됨
                        self.assertTrue(os.path.exists(os.path.join(m, part)), f"{rel}/{part}")
                ws = os.path.join(w, "workspace")
                if os.path.isdir(ws):  # 워크스페이스 연동(③)
                    for rel in ("worker-types.json", "prepare.sh", "panel/panel.json", "panel/panel.js",
                                "config.json", "presets/default.json"):
                        self.assertTrue(os.path.exists(os.path.join(ws, rel)), f"workspace/{rel}")



class Layers(unittest.TestCase):
    def test_core_does_not_use_libworker(self):
        """① 유닛(모듈 코어)은 libworker를 모른다: 다른 프로그램에 core/만 가져가 쓸 수 있어야 한다(design.md 6.6.4)"""
        platform = [d for w in PLATFORM_WORKERS for d in sorted(glob.glob(os.path.join(w, "lib", "*")))
                    if os.path.isdir(os.path.join(d, "core"))]  # 플랫폼 블록 모듈(schema.json 없음)
        self.assertTrue(platform, "monitor/lib/<모듈>/core")
        for m in [m for w in workers(with_template=True) for m in modules(w)] + platform:
            for f in sources(os.path.join(m, "core")):
                text = read(f)
                with self.subTest(file=os.path.relpath(f, ROOT)):
                    self.assertNotIn('#include "worker/', text)
                    self.assertNotIn("#include <worker/", text)


class Compliance(unittest.TestCase):
    def test_report(self):
        """미준수 목록(실패하지 않음): 유닛 시험 = 모듈 test/에서 그 모듈 코어 헤더를 직접 쓰는 시험"""
        issues = []
        for w in workers(with_template=True):
            for m in modules(w):
                name = os.path.basename(m)
                core_headers = [os.path.relpath(h, os.path.join(m, "core", "include")).replace(os.sep, "/")
                                for h in sources(os.path.join(m, "core", "include"))]
                tests = [read(f) for f in sources(os.path.join(m, "test"))]
                if core_headers and not any(('"%s"' % h) in t or ("<%s>" % h) in t for h in core_headers for t in tests):
                    issues.append("%s/lib/%s: 유닛 시험 없음(test/에서 코어 헤더를 쓰는 시험)" % (os.path.basename(w), name))
            doc = os.path.join(w, "new" if os.path.basename(w) == "_template" else "", "design.md")
            if os.path.exists(doc) and "## 3. 인터페이스 명세 (ICD)" not in read(doc):
                issues.append("%s/design.md: 3장 인터페이스 명세(ICD) 없음(틀 new/design.md 형식)" % os.path.basename(w))
            if not os.path.exists(os.path.join(w, "io.json")):
                issues.append("%s: 입출력 정의(io.json, io/) 없음(design.md 6.6.7.1)" % os.path.basename(w))
            else:
                if not os.path.exists(os.path.join(w, "io", "self.json")):
                    issues.append("%s: 워커 정의(io/self.json) 없음(design.md 6.6.7.1)" % os.path.basename(w))
                if "check" not in load_json(os.path.join(w, "io.json")):
                    issues.append("%s: 출력 대조 방법(io.json check) 없음(design.md 6.6.7.2)" % os.path.basename(w))
        issues += IoDefinitions.notices()
        if issues:
            print("\n미준수(워커 고도화 대상):\n  " + "\n  ".join(issues))


IO_NAME = re.compile(r"^[a-z][a-z0-9_]*$")
IO_VERSION = re.compile(r"^(\d+)\.(\d+)$")
# 명령 정의의 칸(design.md 6.6.7.1, docs/command-path.md 3장)
CMD_KEYS = {"description", "args", "reply", "errors", "read", "operate", "lease_ms"}
IO_ERRORS = {"bad_request", "unavailable", "timeout", "busy", "forbidden", "internal"}  # 9.6
SCHEMA_TYPES = {"object", "array", "string", "number", "integer", "boolean", "null"}


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def role_files(w):
    """워커의 역할 정의 원본(io/<역할>.json, 워커 정의 self·공통 정의 common은 빼고)"""
    return sorted(p for p in glob.glob(os.path.join(w, "io", "*.json"))
                  if os.path.splitext(os.path.basename(p))[0] not in ic.NOT_ROLES)


def io_originals():
    """정의 이름 → 원본 파일들(workers/*/io/<역할>.json, 사본 io/uses/는 빼고)"""
    out = {}
    for w in workers(with_template=True):
        for p in role_files(w):
            out.setdefault(os.path.splitext(os.path.basename(p))[0], []).append(p)
    return out


def version_tuple(v):
    m = IO_VERSION.match(v or "")
    return (int(m.group(1)), int(m.group(2))) if m else None


class IoDefinitions(unittest.TestCase):
    """입출력 정의(design.md 6.6.7.1~6.6.7.2). io.json이 있는 워커만 본다(없는 워커는 미준수 목록에 알림)"""

    def check_schema(self, s, where, defs=None):
        self.assertIsInstance(s, dict, "%s: JSON Schema(객체)여야 함" % where)
        if "$ref" in s:  # 정의 파일 안의 공용 모양만: {"$ref": "#/definitions/<이름>"}
            ref = s["$ref"]
            self.assertTrue(isinstance(ref, str) and ref.startswith("#/definitions/") and
                            ref[len("#/definitions/"):] in (defs or {}), "%s: $ref %s가 definitions에 없음" % (where, ref))
            return
        t = s.get("type")
        types = t if isinstance(t, list) else [t]
        self.assertTrue(t is not None and all(x in SCHEMA_TYPES for x in types), "%s: type이 %s 중 하나" % (where, sorted(SCHEMA_TYPES)))
        props = s.get("properties", {})
        self.assertIsInstance(props, dict, "%s: properties는 객체" % where)
        for k, sub in props.items():
            self.check_schema(sub, "%s.%s" % (where, k), defs)
        for k in s.get("required", []):
            self.assertIn(k, props, "%s: required의 %s가 properties에 없음" % (where, k))
        if "items" in s:
            self.check_schema(s["items"], where + "[]", defs)

    def check_definition(self, path):
        d = load_json(path)
        stem = os.path.splitext(os.path.basename(path))[0]
        rel = os.path.relpath(path, ROOT)
        self.assertEqual(sorted(set(d) - {"name", "version", "description", "commands", "topics", "data", "definitions", "safe_state"}), [],
                         rel + ": 모르는 항목")
        defs = d.get("definitions", {})
        self.assertIsInstance(defs, dict, rel + ": definitions는 객체")
        for k, sub in defs.items():
            self.check_schema(sub, "%s: definitions.%s" % (rel, k), defs)
        self.assertEqual(d.get("name"), stem, rel + ": name은 파일 이름과 같아야 함")
        self.assertRegex(stem, IO_NAME, rel + ": 정의 이름 형식")
        self.assertIsNotNone(version_tuple(d.get("version")), rel + ": version은 '큰.작은'")
        for name, c in d.get("commands", {}).items():
            where = "%s: commands.%s" % (rel, name)
            self.assertEqual(sorted(set(c) - CMD_KEYS), [], where + ": 모르는 항목")
            self.check_cmd_flags(c, where)
            self.check_schema(c.get("args"), where + ".args", defs)
            self.check_schema(c.get("reply"), where + ".reply", defs)
            self.assertEqual(sorted(set(c.get("errors", [])) - IO_ERRORS), [], where + ".errors는 9.6 code만")
        # 명령 임대(docs/command-lease.md): lease_ms는 100 이상 정수, 있으면 safe_state가 있어야 하고 그 명령은 같은 정의의 임대 없는 명령
        leased = [n for n, c in d.get("commands", {}).items() if "lease_ms" in c]
        for n in leased:
            v = d["commands"][n]["lease_ms"]
            self.assertTrue(isinstance(v, int) and not isinstance(v, bool) and v >= 100, "%s: commands.%s.lease_ms는 100 이상 정수" % (rel, n))
            self.assertFalse(d["commands"][n].get("read"), "%s: commands.%s: 읽기 명령에는 임대를 붙이지 않음" % (rel, n))
        if leased or "safe_state" in d:
            ss = d.get("safe_state")
            self.assertIsInstance(ss, dict, rel + ": lease_ms가 있으면 safe_state {\"cmd\", \"args\"?}가 있어야 함")
            self.assertEqual(sorted(set(ss) - {"cmd", "args"}), [], rel + ": safe_state: 모르는 항목")
            self.assertIn(ss.get("cmd"), d.get("commands", {}), rel + ": safe_state.cmd는 이 정의의 명령")
            self.assertNotIn("lease_ms", d["commands"][ss["cmd"]], rel + ": safe_state 명령에는 임대를 붙이지 않음")
            if "args" in ss:
                self.assertIsInstance(ss["args"], dict, rel + ": safe_state.args는 객체")
        for name, t in d.get("topics", {}).items():
            where = "%s: topics.%s" % (rel, name)
            self.assertEqual(sorted(set(t) - {"description", "every_ms", "schema"}), [], where + ": 모르는 항목")
            self.assertTrue(isinstance(t.get("every_ms"), (int, float)) and t["every_ms"] > 0, where + ".every_ms > 0")
            self.check_schema(t.get("schema"), where + ".schema", defs)
        for name, b in d.get("data", {}).items():
            where = "%s: data.%s" % (rel, name)
            self.assertEqual(sorted(set(b) - {"description", "format"}), [], where + ": 모르는 항목")
            self.assertIsInstance(b.get("format"), str, where + ".format")
        return d

    def test_io_files(self):
        for w in workers(with_template=True):
            index = os.path.join(w, "io.json")
            if not os.path.exists(index):
                continue
            name = os.path.basename(w)
            with self.subTest(worker=name):
                io = load_json(index)
                self.assertEqual(sorted(set(io) - {"provides", "uses", "check"}), [], "io.json: 모르는 항목")
                for p in sorted(role_files(w) + glob.glob(os.path.join(w, "io", "uses", "*.json"))):
                    self.check_definition(p)
                for d in io.get("provides", []):
                    self.assertTrue(os.path.exists(os.path.join(w, "io", d + ".json")) or
                                    os.path.exists(os.path.join(w, "io", "uses", d + ".json")),
                                    "provides %s: io/%s.json(원본) 또는 io/uses/%s.json(사본)이 없음" % (d, d, d))
                for inp, u in io.get("uses", {}).items():
                    where = "uses.%s" % inp
                    self.assertEqual(sorted(set(u) - {"description", "from", "version", "topics", "commands", "data", "required"}), [],
                                     where + ": 모르는 항목")
                    self.assertIsInstance(u.get("required"), bool, where + ".required(true/false)")
                    copy = os.path.join(w, "io", "uses", "%s.json" % u.get("from"))
                    self.assertTrue(os.path.exists(copy), "%s: 사본 io/uses/%s.json이 없음" % (where, u.get("from")))
                    if not os.path.exists(copy):
                        continue
                    d = load_json(copy)
                    self.assertEqual(u.get("version"), d.get("version"), where + ".version은 가져온 사본의 버전")
                    for kind in ("topics", "commands", "data"):
                        for item in u.get(kind, []):
                            self.assertIn(item, d.get(kind, {}), "%s: %s %s가 정의 %s에 없음" % (where, kind, item, u.get("from")))

    def check_cmd_flags(self, c, where):
        """명령 표시(docs/command-path.md 3장): read(읽기, 늘 됨)·operate(워커 전용이지만 운용 모드에서도 됨)는 true만, 둘을 같이 쓰지 않음"""
        for k in ("read", "operate"):
            if k in c:
                self.assertIs(c[k], True, where + "." + k + "는 true만(없으면 아님)")
        self.assertFalse(c.get("read") and c.get("operate"), where + ": read와 operate를 같이 쓰지 않음")

    def check_parts(self, d, where, defs, allowed):
        """워커 정의·조각·공통 정의의 status·metrics·commands·topics 형식"""
        self.assertEqual(sorted(set(d) - allowed), [], where + ": 모르는 항목")
        for k, sub in d.get("status", {}).items():
            self.check_schema(sub, "%s: status.%s" % (where, k), defs)
        for k, m in d.get("metrics", {}).items():
            self.assertTrue(m.get("type") in ("integer", "number", "string"), "%s: metrics.%s.type은 integer·number·string" % (where, k))
        for name, c in d.get("commands", {}).items():
            self.assertEqual(sorted(set(c) - CMD_KEYS), [], "%s: commands.%s: 모르는 항목" % (where, name))
            self.check_cmd_flags(c, "%s: commands.%s" % (where, name))
            self.check_schema(c.get("args"), "%s: commands.%s.args" % (where, name), defs)
            self.check_schema(c.get("reply"), "%s: commands.%s.reply" % (where, name), defs)
            self.assertEqual(sorted(set(c.get("errors", [])) - IO_ERRORS), [], "%s: commands.%s.errors는 9.6 code만" % (where, name))
        for name, t in d.get("topics", {}).items():
            self.check_schema(t.get("schema"), "%s: topics.%s.schema" % (where, name), defs)

    def test_common_definition(self):
        d = load_json(os.path.join(TEMPLATE, "lib", "libworker", "io", "common.json"))
        self.assertEqual(d.get("name"), "common")
        self.assertEqual(sorted(set(d.get("errors", [])) ^ IO_ERRORS), [], "common.json errors = 9.6 code")
        self.check_parts(d, "common.json", d.get("definitions", {}),
                         {"name", "version", "description", "errors", "status", "metrics", "commands", "definitions", "stream"})
        self.check_schema(d.get("stream"), "common.json: stream", d.get("definitions", {}))
        for k in ("id", "config_version", "runtime"):
            self.assertIn(k, d.get("status", {}), "common.json status." + k)

    def test_library_fragments(self):
        """라이브러리 조각(lib/<라이브러리>/io/<모듈>.json): 형식, 지표 이름은 그 조각의 status 모듈로 시작"""
        allowed = {"name", "version", "description", "status", "metrics", "commands", "topics", "definitions"}
        for p in sorted(glob.glob(os.path.join(ROOT, "workers", "*", "lib", "*", "io", "*.json"))):
            if p.replace(os.sep, "/").endswith("/lib/libworker/io/common.json"):
                continue  # 공통 정의(libworker 안, test_common_definition이 봄)
            rel = os.path.relpath(p, ROOT)
            with self.subTest(fragment=rel):
                d = load_json(p)
                self.assertEqual(d.get("name"), os.path.splitext(os.path.basename(p))[0], rel + ": name은 파일 이름")
                self.assertIsNotNone(version_tuple(d.get("version")), rel + ": version은 '큰.작은'")
                self.check_parts(d, rel, d.get("definitions", {}), allowed)
                for m in d.get("metrics", {}):
                    self.assertTrue(any(m.startswith(k + "_") for k in d.get("status", {})), "%s: 지표 %s는 <모듈>_" % (rel, m))

    def test_self_definitions(self):
        """워커 정의(io/self.json)와 include 조각. 없는 워커는 미준수 목록(알림)"""
        allowed = {"name", "version", "description", "include", "status", "metrics", "commands", "topics", "definitions",
                   "lease_safe_state"}
        for w in workers(with_template=True):
            path = os.path.join(w, "io", "self.json")
            if not os.path.exists(os.path.join(w, "io.json")) or not os.path.exists(path):
                continue
            name = os.path.basename(w)
            with self.subTest(worker=name):
                d = load_json(path)
                self.assertEqual(d.get("name"), "self", name + "/io/self.json: name은 self")
                self.assertIsNotNone(version_tuple(d.get("version")), name + "/io/self.json: version은 '큰.작은'")
                self.check_parts(d, name + "/io/self.json", d.get("definitions", {}), allowed)
                parts = [d]
                for inc in d.get("include", []):
                    p = os.path.join(w, inc)
                    self.assertTrue(os.path.exists(p), "%s/io/self.json include %s: 파일 없음" % (name, inc))
                    if os.path.exists(p):
                        f = load_json(p)
                        self.check_parts(f, "%s/%s" % (name, inc), f.get("definitions", {}), allowed - {"include"})
                        parts.append(f)
                try:
                    c = ic.Contract(w)
                except ic.ContractError as e:
                    self.fail("%s: %s" % (name, e))
                modules_in_status = set(k for f in parts for k in f.get("status", {}))
                for f in parts:
                    for m in f.get("metrics", {}):
                        self.assertTrue(any(m.startswith(mod + "_") for mod in modules_in_status),
                                        "%s: 지표 %s는 <모듈>_<이름>(모듈은 status에 있는 것)" % (name, m))
                check = c.io.get("check")
                if check:
                    self.assertTrue(os.path.exists(os.path.join(w, "workspace", "presets", "%s.json" % check.get("preset"))),
                                    "%s: io.json check.preset %s 프리셋 없음" % (name, check.get("preset")))
                    self.assertTrue(isinstance(check.get("listen_s"), (int, float)) and check["listen_s"] > 0,
                                    name + ": check.listen_s > 0")
                    for call in check.get("calls", []):
                        self.assertIsNotNone(c.commands.find(call.get("cmd"))[1],
                                             "%s: check.calls %s가 정의에 없음" % (name, call.get("cmd")))

    def test_one_original_per_definition(self):
        for d, paths in io_originals().items():
            with self.subTest(definition=d):
                self.assertEqual(len(paths), 1, "정의 원본은 하나(나머지 워커는 io/uses/ 사본): %s" % [os.path.relpath(p, ROOT) for p in paths])

    def test_copies_match_original(self):
        """사본이 원본과 다르면: 큰 버전이 같고 원본이 더 새것이면 알림(notices), 그 밖에는 실패"""
        originals = io_originals()
        for w in workers(with_template=True):
            for copy in sorted(glob.glob(os.path.join(w, "io", "uses", "*.json"))):
                d = os.path.splitext(os.path.basename(copy))[0]
                if len(originals.get(d, [])) != 1:
                    continue  # 원본이 다른 저장소에 있음
                orig = originals[d][0]
                if filecmp.cmp(orig, copy, shallow=False):
                    continue
                vo, vc = version_tuple(load_json(orig).get("version")), version_tuple(load_json(copy).get("version"))
                with self.subTest(copy=os.path.relpath(copy, ROOT)):
                    self.assertTrue(vo and vc and vo[0] == vc[0] and vo > vc,
                                    "원본 %s(%s)과 다름: 같은 버전이면 내용이 같아야 하고, 큰 버전이 다르면 이어지지 않음" %
                                    (os.path.relpath(orig, ROOT), load_json(orig).get("version")))

    @staticmethod
    def notices():
        out = []
        originals = io_originals()
        for w in workers(with_template=True):
            for copy in sorted(glob.glob(os.path.join(w, "io", "uses", "*.json"))):
                d = os.path.splitext(os.path.basename(copy))[0]
                if len(originals.get(d, [])) == 1 and not filecmp.cmp(originals[d][0], copy, shallow=False):
                    out.append("%s: 원본(%s)이 더 새것, 사본을 새로 가져오기" % (os.path.relpath(copy, ROOT), os.path.relpath(originals[d][0], ROOT)))
        return out


class Panels(unittest.TestCase):
    def test_panel_ids_unique(self):
        """화면 패널 id가 겹치면 하나가 가려진다: 공용 패널(ui/web/panels/<폴더>)과 워커 패널(panel.json "id")"""
        seen = {}
        for d in sorted(glob.glob(os.path.join(ROOT, "ui", "web", "panels", "*"))):
            if os.path.isfile(os.path.join(d, "panel.json")):
                seen[os.path.basename(d)] = os.path.relpath(d, ROOT)
        for w in workers():
            manifest = os.path.join(w, "workspace", "panel", "panel.json")
            if not os.path.isfile(manifest):
                continue
            with open(manifest, encoding="utf-8") as f:
                pid = json.load(f).get("id") or os.path.basename(w)
            with self.subTest(panel=pid, worker=os.path.basename(w)):
                self.assertNotIn(pid, seen, "패널 id가 %s와 겹침" % seen.get(pid))
            seen[pid] = os.path.relpath(manifest, ROOT)


class PythonHelper(unittest.TestCase):
    def test_python_helper_copies_match_template(self):
        """Python 워커 도우미(workspace_worker.py)의 사본은 원본(workers/_template/python/)과 같아야 한다"""
        orig = os.path.join(TEMPLATE, "python", "workspace_worker.py")
        copies = [p for p in glob.glob(os.path.join(ROOT, "**", "workspace_worker.py"), recursive=True)
                  if os.path.abspath(p) != os.path.abspath(orig) and "build" not in p.split(os.sep)]
        # 사본이 없어도 된다(시스템 모니터가 C++로 바뀌어 지금은 Python 워커가 없음)
        for p in copies:
            with self.subTest(copy=os.path.relpath(p, ROOT)):
                self.assertTrue(filecmp.cmp(orig, p, shallow=False))


if __name__ == "__main__":
    unittest.main()
