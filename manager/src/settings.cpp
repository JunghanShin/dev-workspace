#include "settings.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "log.hpp"
#include "posix_fs.hpp"
#include "supervisor.hpp"  // normpath

namespace pm {

namespace {

PosixFs g_fs;

Json defaults() {
    return Json::parse(R"({
        "api": {"read_timeout_s": 1.0, "max_message_bytes": 65536, "subscribe_period_ms": 1000,
                "subscriber_timeout_s": 5.0},
        "restart": {"interval_s": 2.0, "max_interval_s": 30.0, "healthy_s": 60.0, "crash_window_s": 300.0,
                    "crash_count": 5},
        "worker_defaults": {"start_timeout_sec": 10, "watchdog_sec": 3, "stop_timeout_sec": 5},
        "log": {"max_bytes": 10485760, "backups": 3},
        "tick_s": 0.2
    })");
}

void merge(Json& base, const Json& over) {
    for (auto it = over.begin(); it != over.end(); ++it) {
        if (it->is_object() && base.contains(it.key()) && base[it.key()].is_object()) merge(base[it.key()], *it);
        else base[it.key()] = *it;
    }
}

std::string join_path(const std::string& a, const std::string& b) {
    if (!b.empty() && b[0] == '/') return b;
    if (a.empty() || a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string base_name(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string dir_name(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? "" : (s == 0 ? "/" : p.substr(0, s));
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::optional<Json> read_json_file(const std::string& path, std::string& err) {
    std::string text;
    const auto r = g_fs.read(path, text, err);
    if (r == FileSystem::Read::Missing) {
        err = errno_msg(2, path);
        return std::nullopt;
    }
    if (r == FileSystem::Read::Error) return std::nullopt;
    return parse(text, &err);
}

// worker-types.d/<워커 폴더>.json → 워커 폴더 설정 자리(workspace/, 예전 워커는 폴더 맨 위). 없으면 ""
std::string worker_conf_dir(const std::string& root, const std::string& folder) {
    const std::string w = join_path(join_path(root, "workers"), folder);
    if (!g_fs.is_dir(w)) return "";
    const std::string ws = w + "/workspace";
    return g_fs.is_dir(ws) ? ws : w;
}

bool which(const std::string& name) {
    auto is_exec = [](const std::string& p) { return g_fs.is_file(p) && ::access(p.c_str(), X_OK) == 0; };
    if (name.find('/') != std::string::npos) return is_exec(name);
    const char* path = std::getenv("PATH");
    std::string ps = path ? path : "/bin:/usr/bin";
    size_t i = 0;
    while (i <= ps.size()) {
        size_t j = ps.find(':', i);
        if (j == std::string::npos) j = ps.size();
        const std::string d = ps.substr(i, j - i);
        if (is_exec((d.empty() ? std::string(".") : d) + "/" + name)) return true;
        i = j + 1;
    }
    return false;
}

}  // namespace

std::string abspath(const std::string& p) {
    if (!p.empty() && p[0] == '/') return normpath(p);
    char buf[4096];
    const char* cwd = ::getcwd(buf, sizeof buf);
    return normpath(std::string(cwd ? cwd : "/") + "/" + p);
}

Json load_settings(const std::string& path, const std::string& data_dir) {
    Json settings = defaults();
    if (!path.empty()) {
        std::string text, err;
        const auto r = g_fs.read(path, text, err);
        if (r == FileSystem::Read::Missing) {
            PM_WARN("settings %s not found, using defaults", path.c_str());
        } else if (r == FileSystem::Read::Error) {
            PM_ERROR("settings %s unusable (%s), using defaults", path.c_str(), err.c_str());
        } else {
            auto j = parse(text, &err);
            if (!j) PM_ERROR("settings %s unusable (%s), using defaults", path.c_str(), err.c_str());
            else if (!j->is_object()) PM_ERROR("settings %s unusable (must be an object), using defaults", path.c_str());
            else merge(settings, *j);
        }
    }
    const std::vector<std::pair<const char*, const char*>> paths = {
        {"state_dir", "state"},
        {"run_dir", "run"},
        // 예전 워커 구성 파일. 처음 한 번 워커 폴더로 옮길 때만 읽는다(6.3.1)
        {"workers_file", "config/workers.json"},
        {"types_file", "config/worker-types.json"},
        {"types_dir", "config/worker-types.d"},
    };
    for (const auto& [key, rel] : paths) {
        if (!settings.contains(key) || settings[key].is_null()) {  // 빈 문자열은 "사용 안 함"
            if (data_dir.empty())
                throw std::runtime_error(std::string(key) + " not set and WORKSPACE_DATA not given");
            settings[key] = join_path(data_dir, rel);
        }
    }
    return settings;
}

Json load_types(const std::string& path, const std::string& dir, const Json& worker_defaults, const std::string& root) {
    std::vector<std::string> files;
    if (!path.empty() && g_fs.exists(path)) files.push_back(path);
    if (!dir.empty() && g_fs.is_dir(dir)) {
        std::vector<std::string> names;
        for (const auto& n : g_fs.list(dir))
            if (ends_with(n, ".json")) names.push_back(n);
        std::sort(names.begin(), names.end());
        for (const auto& n : names) files.push_back(join_path(dir, n));
    }
    Json raw = Json::object();
    for (const auto& file : files) {
        // 깨진 조각 하나 때문에 Manager가 뜨지 못하면 안 된다. 그 파일만 건너뛴다
        std::string err;
        auto entries = read_json_file(file, err);
        if (!entries || !entries->is_object()) {
            PM_ERROR("worker types %s skipped: %s", file.c_str(), entries ? "must be an object" : err.c_str());
            continue;
        }
        // worker-types.d/<워커 폴더 이름>.json(prepare.sh가 만든 조각)이면 그 워커 폴더를 기억한다
        const bool in_dir = !dir.empty() && dir_name(file) == dir;
        const std::string folder = in_dir ? base_name(file).substr(0, base_name(file).size() - 5) : "";
        for (auto it = entries->begin(); it != entries->end(); ++it) {
            if (raw.contains(it.key())) PM_WARN("worker type '%s' redefined in %s", it.key().c_str(), file.c_str());
            Json entry = *it;
            if (entry.is_object() && in_dir) entry["_folder"] = folder;
            raw[it.key()] = entry;
        }
    }
    if (raw.empty()) PM_WARN("no worker types registered");
    Json types = Json::object();
    for (auto it = raw.begin(); it != raw.end(); ++it) {
        const std::string name = it.key();
        if (!it->is_object()) {
            PM_ERROR("worker type '%s' skipped: must be an object", name.c_str());
            continue;
        }
        Json t = worker_defaults;
        for (auto kv = it->begin(); kv != it->end(); ++kv) t[kv.key()] = *kv;
        if (t.contains("exec") && t["exec"].is_string()) t["exec"] = Json::array({t["exec"]});
        if (t.contains("exec_candidates") && !t["exec_candidates"].is_array()) t.erase("exec_candidates");
        if (!py_truthy(t.value("exec", Json()))) {
            PM_ERROR("worker type '%s' skipped: exec missing", name.c_str());
            continue;
        }
        t.erase("role");  // 예전 선언의 role은 쓰지 않는다(워커끼리는 독립)
        const Json folder = t.value("_folder", Json());
        t.erase("_folder");
        const std::string conf = py_truthy(folder) && folder.is_string() ? worker_conf_dir(root, folder.get<std::string>()) : "";
        if (!conf.empty()) {
            // 워커 폴더 하나 = 워커 하나. id는 폴더 이름(6.3.1)
            bool dup = false;
            for (auto x = types.begin(); x != types.end(); ++x)
                dup = dup || x->value("_worker_id", Json()) == folder;
            if (dup) {
                PM_ERROR("worker type '%s' skipped: worker folder %s already defines a type", name.c_str(),
                         folder.get<std::string>().c_str());
                continue;
            }
            t["_worker_id"] = folder;
            t["_conf_dir"] = conf;
        }
        types[name] = t;
    }
    return types;
}

void add_builtin_types(Json& types, const Json& worker_defaults, const std::string& root) {
    // <root>/*/worker-types.json. exec 안의 @WORKSPACE_ROOT@는 워크스페이스 경로로 바꾼다
    std::vector<std::string> paths;
    for (const auto& n : g_fs.list(root)) {
        if (n.empty() || n[0] == '.') continue;  // glob "*"은 숨은 폴더를 빼고
        const std::string p = join_path(join_path(root, n), "worker-types.json");
        if (g_fs.is_file(p)) paths.push_back(p);
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        std::string text, err;
        if (g_fs.read(path, text, err) != FileSystem::Read::Ok) {
            PM_ERROR("built-in types %s skipped: %s", path.c_str(), err.c_str());
            continue;
        }
        for (size_t pos = 0; (pos = text.find("@WORKSPACE_ROOT@", pos)) != std::string::npos; pos += root.size())
            text.replace(pos, 16, root);
        auto entries = parse(text, &err);
        if (!entries || !entries->is_object()) {
            PM_ERROR("built-in types %s skipped: %s", path.c_str(), entries ? "must be an object" : err.c_str());
            continue;
        }
        for (auto it = entries->begin(); it != entries->end(); ++it) {
            if (!it->is_object() || !py_truthy(it->value("exec", Json()))) {
                PM_ERROR("built-in type '%s' in %s skipped: needs exec", it.key().c_str(), path.c_str());
                continue;
            }
            Json t = worker_defaults;
            for (auto kv = it->begin(); kv != it->end(); ++kv) t[kv.key()] = *kv;
            t.erase("role");
            const Json b = t.value("builtin", Json());
            if (b.is_object() && b.value("id", Json()).is_string()) {
                t["_worker_id"] = b["id"];
                t["_conf_dir"] = dir_name(path);  // 부품 폴더 맨 위에 config.json·presets/
            }
            if (t["exec"].is_string()) t["exec"] = Json::array({t["exec"]});
            if (!types.contains(it.key())) types[it.key()] = t;
        }
    }
}

void mark_requirements(Json& types) {
    for (auto it = types.begin(); it != types.end(); ++it) {
        bool ok = true;
        const Json req = it->value("requires", Json::array());
        if (req.is_array())
            for (const auto& x : req) {
                const std::string n = py_str(x);
                ok = ok && (which(n) || g_fs.exists("/usr/bin/" + n));
            }
        (*it)["_requires_met"] = ok;
    }
}

std::set<std::string> manager_api_workers(const Json& types) {
    std::set<std::string> out;
    for (auto it = types.begin(); it != types.end(); ++it) {
        if (it->value("manager_api", Json()) != Json(true)) continue;
        const Json wid = it->value("_worker_id", Json());
        if (!wid.is_string() || wid.get<std::string>().empty() || wid == Json("rest") || wid == Json("logger")) {
            PM_ERROR("worker type '%s': manager_api ignored (worker id %s)", it.key().c_str(), py_repr(wid).c_str());
            continue;
        }
        out.insert(wid.get<std::string>());
    }
    return out;
}

}  // namespace pm
