"""워커 설정 파일: 워커 폴더에 있다 (design.md 6.3.1).

워크스페이스는 워커 목록도 워커 설정도 들고 있지 않다. 워커 폴더가 곧 워커이고, 설정은 그 폴더의 파일이다.
깃에 함께 올라가서 다른 보드·PC에서 받아도 그대로다.

  <워커 폴더>/workspace/config.json         {"active": "<프리셋>", "autostart": false}
  <워커 폴더>/workspace/presets/<이름>.json  {"label": "<화면 이름>", "config": {...}}

기본 탑재 워커(플랫폼 부품, 예: monitor/)는 부품 폴더 맨 위에 같은 파일을 둔다.
쓰기는 임시 파일 → rename(반쯤 쓴 파일이 남지 않게).
"""
import json
import os
import re

NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,31}$")
DEFAULT_PRESET = "default"


class WorkerFileError(Exception):
    pass


def _read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as e:
        raise WorkerFileError("%s: %s" % (path, e))


def _write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(obj, ensure_ascii=False, indent=2) + "\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def check_name(name):
    if not isinstance(name, str) or not NAME_RE.match(name):
        raise WorkerFileError("preset name must match %s" % NAME_RE.pattern)
    return name


class WorkerFiles:
    def __init__(self, conf_dir):
        self.dir = conf_dir
        self.config_path = os.path.join(conf_dir, "config.json")
        self.presets_dir = os.path.join(conf_dir, "presets")

    def preset_path(self, name):
        return os.path.join(self.presets_dir, check_name(name) + ".json")

    # ---- config.json --------------------------------------------------

    def read_config(self):
        """{"active", "autostart"} 또는 None(파일 없음)"""
        data = _read_json(self.config_path)
        if data is None:
            return None
        if not isinstance(data, dict):
            raise WorkerFileError("%s must be an object" % self.config_path)
        active = data.get("active", DEFAULT_PRESET)
        autostart = data.get("autostart", False)
        if not isinstance(active, str) or not NAME_RE.match(active):
            raise WorkerFileError("%s: active must be a preset name" % self.config_path)
        if not isinstance(autostart, bool):
            raise WorkerFileError("%s: autostart must be true or false" % self.config_path)
        return {"active": active, "autostart": autostart}

    def write_config(self, active, autostart):
        _write_json(self.config_path, {"active": active, "autostart": autostart})

    # ---- presets/ -----------------------------------------------------

    def has_preset(self, name):
        return os.path.isfile(self.preset_path(name))

    def read_preset(self, name):
        """{"label", "config"}. 없으면 None"""
        path = self.preset_path(name)
        data = _read_json(path)
        if data is None:
            return None
        if not isinstance(data, dict) or not isinstance(data.get("config", {}), dict):
            raise WorkerFileError('%s must be {"label": ..., "config": {...}}' % path)
        return {"label": data.get("label") or name, "config": data.get("config", {})}

    def presets(self):
        """[{"name", "label"}] 이름순. 읽지 못하는 파일은 label에 표시"""
        out = []
        try:
            names = sorted(n[:-5] for n in os.listdir(self.presets_dir) if n.endswith(".json"))
        except FileNotFoundError:
            return out
        for name in names:
            if not NAME_RE.match(name):
                continue
            try:
                p = self.read_preset(name)
                out.append({"name": name, "label": p["label"] if p else name})
            except WorkerFileError:
                out.append({"name": name, "label": name + " (읽지 못함)"})
        return out

    def write_preset(self, name, label, config):
        _write_json(self.preset_path(name), {"label": label or name, "config": config})

    def delete_preset(self, name):
        try:
            os.remove(self.preset_path(name))
        except FileNotFoundError:
            pass

    def rename_preset(self, name, new_name):
        os.replace(self.preset_path(name), self.preset_path(new_name))
