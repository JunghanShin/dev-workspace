#include "worker_files.hpp"

#include <algorithm>
#include <regex>

namespace pm {

const char* const kDefaultPreset = "default";
const char* const kNamePattern = "^[a-z0-9][a-z0-9_-]{0,31}$";

bool name_ok(const std::string& name) {
    static const std::regex re(kNamePattern);
    return std::regex_match(name, re);
}

void check_name(const Json& name) {
    if (!name.is_string() || !name_ok(name.get<std::string>()))
        throw WorkerFileError(std::string("preset name must match ") + kNamePattern);
}

WorkerFiles::WorkerFiles(FileSystem& fs, std::string dir)
    : fs_(fs), dir_(std::move(dir)), config_path_(dir_ + "/config.json"), presets_dir_(dir_ + "/presets") {}

std::string WorkerFiles::preset_path(const Json& name) const {
    check_name(name);
    return presets_dir_ + "/" + name.get<std::string>() + ".json";
}

std::optional<Json> WorkerFiles::read_json(const std::string& path) const {
    std::string text, err;
    switch (fs_.read(path, text, err)) {
    case FileSystem::Read::Missing: return std::nullopt;
    case FileSystem::Read::Error: throw WorkerFileError(path + ": " + err);
    case FileSystem::Read::Ok: break;
    }
    auto j = parse(text, &err);
    if (!j) throw WorkerFileError(path + ": " + err);
    return j;
}

std::optional<WorkerConfig> WorkerFiles::read_config() const {
    auto data = read_json(config_path_);
    if (!data) return std::nullopt;
    if (!data->is_object()) throw WorkerFileError(config_path_ + " must be an object");
    const Json active = data->value("active", Json(kDefaultPreset));
    const Json autostart = data->value("autostart", Json(false));
    if (!active.is_string() || !name_ok(active.get<std::string>()))
        throw WorkerFileError(config_path_ + ": active must be a preset name");
    if (!autostart.is_boolean()) throw WorkerFileError(config_path_ + ": autostart must be true or false");
    return WorkerConfig{active.get<std::string>(), autostart.get<bool>()};
}

FileOp WorkerFiles::write_config(const std::string& active, bool autostart) const {
    Json obj = Json::object();
    obj["active"] = active;
    obj["autostart"] = autostart;
    FileOp op;
    op.path = config_path_;
    op.text = dump_file(obj);
    return op;
}

bool WorkerFiles::has_preset(const Json& name) const { return fs_.is_file(preset_path(name)); }

std::optional<Preset> WorkerFiles::read_preset(const Json& name) const {
    const std::string path = preset_path(name);
    auto data = read_json(path);
    if (!data) return std::nullopt;
    const Json config = data->is_object() ? data->value("config", Json::object()) : Json();
    if (!data->is_object() || !config.is_object())
        throw WorkerFileError(path + " must be {\"label\": ..., \"config\": {...}}");
    const Json label = data->value("label", Json());
    return Preset{py_truthy(label) ? label : name, config};
}

Json WorkerFiles::presets() const {
    Json out = Json::array();
    bool missing = false;
    std::vector<std::string> names;
    for (const auto& n : fs_.list(presets_dir_, &missing))
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) names.push_back(n.substr(0, n.size() - 5));
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        if (!name_ok(name)) continue;
        Json item = Json::object();
        item["name"] = name;
        try {
            auto p = read_preset(name);
            item["label"] = p ? p->label : Json(name);
        } catch (const WorkerFileError&) {
            item["label"] = name + " (읽지 못함)";
        }
        out.push_back(item);
    }
    return out;
}

FileOp WorkerFiles::write_preset(const Json& name, const Json& label, const Json& config) const {
    Json obj = Json::object();
    obj["label"] = py_truthy(label) ? label : name;
    obj["config"] = config;
    FileOp op;
    op.path = preset_path(name);
    op.text = dump_file(obj);
    return op;
}

FileOp WorkerFiles::delete_preset(const Json& name) const {
    FileOp op;
    op.kind = FileOp::Kind::Remove;
    op.path = preset_path(name);
    op.missing_ok = true;
    return op;
}

FileOp WorkerFiles::rename_preset(const Json& name, const Json& new_name) const {
    FileOp op;
    op.kind = FileOp::Kind::Rename;
    op.path = preset_path(name);
    op.to = preset_path(new_name);
    return op;
}

}  // namespace pm
