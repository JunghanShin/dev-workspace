#include "worker/config.hpp"

#include <fstream>

namespace worker {

namespace {

std::string file_stem(const std::string& path) {
    const size_t slash = path.rfind('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

}  // namespace

void require(bool ok, const std::string& message) {
    if (!ok) throw ConfigError(message);
}

ConfigFile load_config_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw ConfigError("cannot open " + path);
    Json doc;
    try {
        doc = Json::parse(in);
    } catch (const Json::parse_error& e) {
        throw ConfigError("bad JSON in " + path + ": " + e.what());
    }
    require(doc.is_object(), "config file must be an object");

    ConfigFile f;
    if (doc.contains("id")) {
        require(doc["id"].is_string() && !doc["id"].get<std::string>().empty(), "id must be a non-empty string");
        f.id = doc["id"].get<std::string>();
    } else if (!worker_id_from_environment().empty()) {
        f.id = worker_id_from_environment();
    } else {
        f.id = file_stem(path);
    }
    require(!f.id.empty() && f.id.find('/') == std::string::npos, "bad worker id: " + f.id);
    if (doc.contains("config_version") && doc["config_version"].is_number_integer()) {
        f.config_version = doc["config_version"].get<long>();
    }
    if (doc.contains("config")) {
        require(doc["config"].is_object(), "config must be an object");
        f.config = doc["config"];
    }
    return f;
}

void check_keys(const Json& config, const std::set<std::string>& known) {
    for (const auto& item : config.items()) {
        if (!known.count(item.key())) throw ConfigError("unknown config key: " + item.key());
    }
}

}  // namespace worker
