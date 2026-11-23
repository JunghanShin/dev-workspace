"""저장소 설정 파일 점검(표준 라이브러리만): 워커 스키마 형식, 프리셋이 스키마를 통과하는지, libvideo 칸 사본, 잘못된 값 검출.
기준 코드는 reference_schema.py(옛 Python Manager의 스키마 검사, 0.58에 Manager를 지울 때 점검 기준으로만 남김).
같은 규칙의 C++ 판은 process-manager core/schema(test_core가 같은 저장소 파일을 검사).

    cd manager && python3 -m unittest test_schema -v
"""
import glob
import json
import os
import unittest

from reference_schema import check_config, check_schema, fields_of, worker_refs

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def worker_types():
    """{type: 선언} (workers/*/worker-types.json)"""
    out = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "workers", "*", "worker-types.json")) +
                       glob.glob(os.path.join(ROOT, "workers", "*", "workspace", "worker-types.json"))):
        out.update(load(path))
    return out


class SchemaFiles(unittest.TestCase):
    def test_worker_schemas_are_well_formed(self):
        types = worker_types()
        self.assertTrue(types)
        for name, decl in types.items():
            with self.subTest(type=name):
                self.assertIn("config_schema", decl)
                self.assertEqual(check_schema(decl["config_schema"]), [])

    def test_stream_fields_match_libvideo(self):
        """송출 항목 칸은 libvideo 원본(stream-schema.json)과 같아야 한다(워커마다 사본)"""
        orig = load(os.path.join(ROOT, "workers/_template/lib/libvideo/stream-schema.json"))["fields"]
        for name, decl in worker_types().items():
            for f in fields_of(decl["config_schema"]):
                if f["key"] == "streams" and f["type"] == "list":
                    with self.subTest(type=name):
                        self.assertEqual(f["fields"], orig)

    def test_module_schema_matches_worker_types(self):
        """모듈 원본 lib/<모듈>/schema.json의 묶음이 그 워커 workspace/worker-types.json에 section을 붙여 그대로 있어야 한다"""
        paths = sorted(glob.glob(os.path.join(ROOT, "workers", "*", "lib", "*", "schema.json")))
        self.assertTrue(paths)
        for path in paths:
            worker_dir = os.path.dirname(os.path.dirname(os.path.dirname(path)))
            types_path = os.path.join(worker_dir, "workspace", "worker-types.json")
            if not os.path.exists(types_path):
                continue  # 워크스페이스 연동을 뗀 워커
            mod = load(path)
            sec = mod["section"]
            want = mod["groups"] if "groups" in mod else [{"fields": mod["fields"]}]
            with self.subTest(module=os.path.relpath(path, ROOT)):
                got = [g for decl in load(types_path).values() for g in decl["config_schema"]["groups"]
                       if g.get("section") == sec]
                self.assertEqual(len(got), len(want), "묶음 개수")
                for g, w in zip(got, want):
                    if "groups" in mod:
                        self.assertEqual({k: v for k, v in g.items() if k != "section"}, w)
                    else:
                        self.assertEqual(g["fields"], w["fields"])

    def test_presets_pass_schema(self):
        """워커 폴더의 프리셋(presets/*.json)이 그 워커 스키마를 통과해야 한다(design.md 6.3.1)"""
        found = 0
        for types_path in sorted(glob.glob(os.path.join(ROOT, "workers", "*", "workspace", "worker-types.json"))):
            ws = os.path.dirname(types_path)
            schemas = [d.get("config_schema") for d in load(types_path).values()]
            for preset in sorted(glob.glob(os.path.join(ws, "presets", "*.json"))):
                found += 1
                data = load(preset)
                with self.subTest(preset=os.path.relpath(preset, ROOT)):
                    self.assertIsInstance(data.get("config"), dict)
                    for schema in schemas:
                        self.assertEqual(check_config(schema, data["config"]), {})
            cfg = os.path.join(ws, "config.json")
            if os.path.exists(cfg):
                c = load(cfg)
                with self.subTest(config=os.path.relpath(cfg, ROOT)):
                    self.assertTrue(os.path.exists(os.path.join(ws, "presets", c.get("active", "default") + ".json")))
                    self.assertIsInstance(c.get("autostart", False), bool)
        self.assertTrue(found)


# 값 검사용 예시 스키마(장치 워커의 칸 모양: 카메라 구동기, 영상 카메라, 네트워크 장치). 특정 워커에 기대지 않는다
STREAM_FIELDS = load(os.path.join(ROOT, "workers/_template/lib/libvideo/stream-schema.json"))["fields"]
MOUNT = {"groups": [
    {"title": "카메라", "section": "cam1", "fields": [
        {"key": "camera_ip", "type": "string", "format": "ipv4", "default": "192.168.144.25"},
        {"key": "center_pitch", "type": "number", "min": -90, "max": 90, "default": 0},
        {"key": "video_bitrate_kbps", "type": "int", "min": 256, "max": 20000},
        {"key": "time_sync", "type": "bool", "default": True},
        {"key": "video_codec", "type": "enum", "options": ["h264", "h265"]}]},
    {"title": "송출", "section": "stream", "fields": [
        {"key": "streams", "type": "list", "item_key": "name", "fields": STREAM_FIELDS}]}]}
CAMERA = {"groups": [{"title": "카메라", "section": "camera", "fields": [
    {"key": "downsampling", "type": "enum", "options": [[1, "원본"], [2, "2×2 비닝"]], "default": 1}]}]}
LINK = {"groups": [{"title": "장치 링크", "section": "link", "fields": [
    {"key": "listen", "type": "string", "format": "host_port", "default": "127.0.0.1:9000"},
    {"key": "streams", "type": "map", "key_pattern": "^[A-Z][A-Z0-9_]*$", "value_type": "number", "min": 0, "max": 500}]}]}


class Values(unittest.TestCase):
    def setUp(self):
        for sch in (MOUNT, CAMERA, LINK):
            self.assertEqual(check_schema(sch), [])
        self.mount, self.camera, self.link = MOUNT, CAMERA, LINK

    def test_empty_config_is_ok(self):
        self.assertEqual(check_config(self.mount, {}), {})

    def test_unknown_keys_are_kept_unchecked(self):
        self.assertEqual(check_config(self.mount, {"command_socket": "/tmp/x.sock"}), {})

    def test_scalar_errors(self):
        errs = check_config(self.mount, {"cam1": {"camera_ip": "192.168.1", "center_pitch": 120,
                                                 "video_bitrate_kbps": 1.5, "time_sync": "yes",
                                                 "video_codec": "mjpeg"}})
        self.assertEqual(set(errs), {"cam1.camera_ip", "cam1.center_pitch", "cam1.video_bitrate_kbps",
                                     "cam1.time_sync", "cam1.video_codec"})

    def test_enum_numeric_and_bool(self):
        self.assertEqual(check_config(self.camera, {"camera": {"downsampling": 2}}), {})
        self.assertIn("camera.downsampling", check_config(self.camera, {"camera": {"downsampling": True}}))
        self.assertIn("camera.downsampling", check_config(self.camera, {"camera": {"downsampling": 3}}))

    def test_list_items(self):
        errs = check_config(self.mount, {"stream": {"streams": [
            {"name": "main", "url": "rtsp://127.0.0.1:8554/cam1"},
            {"name": "main", "url": "http://x"},
            {"url": "rtsp://a:1/b", "bitrate_kbps": 10},
        ]}})
        self.assertEqual(set(errs), {"stream.streams[1].name", "stream.streams[1].url", "stream.streams[2].name",
                                     "stream.streams[2].bitrate_kbps"})

    def test_map(self):
        self.assertEqual(check_config(self.link, {"link": {"streams": {"STATUS": 10, "POSITION": 1}}}), {})
        errs = check_config(self.link, {"link": {"streams": {"status": 10, "POSITION": 900}}})
        self.assertEqual(set(errs), {"link.streams.status", "link.streams.POSITION"})

    def test_sections(self):
        sch = {"groups": [
            {"title": "모듈", "section": "link", "fields": [{"key": "listen", "type": "string", "format": "host_port"}]},
            {"title": "호스트", "fields": [{"key": "watchdog_sec", "type": "number", "min": 1}]}]}
        self.assertEqual(check_schema(sch), [])
        self.assertEqual(check_config(sch, {"link": {"listen": "127.0.0.1:1"}, "watchdog_sec": 3}), {})
        self.assertEqual(set(check_config(sch, {"link": {"listen": "x"}, "watchdog_sec": 0})), {"link.listen", "watchdog_sec"})
        self.assertEqual(set(check_config(sch, {"link": 5})), {"link"})
        self.assertEqual(check_config(sch, {}), {})
        self.assertTrue(check_schema({"groups": [{"section": "Bad-Name", "fields": []}]}))

    def test_host_port(self):
        self.assertEqual(check_config(self.link, {"link": {"listen": "127.0.0.1:9000"}}), {})
        for bad in ("127.0.0.1", ":9000", "host:0", "host:99999"):
            self.assertIn("link.listen", check_config(self.link, {"link": {"listen": bad}}), bad)

    def test_worker_id_and_refs(self):
        """상대 워커 칸(format worker_id): 형식 검사, 상태 peers(빈 칸은 빠짐, 목록 안도 찾음) — design.md 6.6.7"""
        schema = {"groups": [
            {"section": "peer", "fields": [{"key": "peer", "type": "string", "format": "worker_id"}]},
            {"section": "multi", "fields": [{"key": "links", "type": "list", "item_key": "to", "fields": [
                {"key": "to", "type": "string", "format": "worker_id"}]}]}]}
        self.assertEqual(check_config(schema, {"peer": {"peer": "cam1"}}), {})
        self.assertEqual(check_config(schema, {"peer": {"peer": ""}}), {})
        for bad in ("Cam1", "a b", "../x"):
            self.assertIn("peer.peer", check_config(schema, {"peer": {"peer": bad}}), bad)
        cfg = {"peer": {"peer": "cam1"}, "multi": {"links": [{"to": "cam2"}, {"to": ""}]}}
        self.assertEqual(worker_refs(schema, cfg), {"peer.peer": "cam1", "multi.links[0].to": "cam2"})
        self.assertEqual(worker_refs(schema, {"peer": {"peer": ""}}), {})


if __name__ == "__main__":
    unittest.main()
