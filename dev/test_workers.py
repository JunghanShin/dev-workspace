"""워커 폴더 구조 검사(표준 라이브러리만, 워크스페이스 design.md 3장 "단계"와 6.6). 템플릿과 모든 워커를 본다:
  - 공용 라이브러리 사본(lib/lib*)이 틀(workers/_template)의 원본과 같은지
  - 폴더 구성: 워커(CMake·README·design·main·workspace/), 모듈(core/include·include·src·test·README·schema)
  - 층 규칙: 모듈 코어(① 유닛)가 libworker를 쓰지 않는지(쓰면 실패)
  - 패널 id 겹침, Python 도우미 사본
  - 미준수 목록(실패가 아니라 알림): 유닛 시험이 없는 모듈, ICD(설계서 3장)가 없는 워커 → 워커 고도화 때 하나씩 없앤다

    python3 -m unittest dev/test_workers.py -v      (워크스페이스 폴더에서)

사본이 원본과 다르면: 원본을 고친 것이면 그 워커에 덮어쓰고 그 워커를 시험한다. 워커 쪽만 고친 것이면 원본(틀)에 먼저 반영한다.
"""
import filecmp
import glob
import json
import os
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TEMPLATE = os.path.join(ROOT, "workers", "_template")
# 공용 라이브러리 = 틀에 있는 lib/lib*(원본). 워커는 쓰는 것만 사본으로 둔다. 틀의 예제 모듈(source 등)은 출발점이라 비교하지 않는다
COPIED_LIBS = tuple(sorted(os.path.basename(d) for d in glob.glob(os.path.join(TEMPLATE, "lib", "lib*")) if os.path.isdir(d)))
SKIP_DIRS = {"build", ".git", "__pycache__"}


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
        for w in workers():
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
        for w in workers(with_template=True):
            for m in modules(w):
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
        if issues:
            print("\n미준수(워커 고도화 대상):\n  " + "\n  ".join(issues))


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
        self.assertTrue(copies)
        for p in copies:
            with self.subTest(copy=os.path.relpath(p, ROOT)):
                self.assertTrue(filecmp.cmp(orig, p, shallow=False))


if __name__ == "__main__":
    unittest.main()
