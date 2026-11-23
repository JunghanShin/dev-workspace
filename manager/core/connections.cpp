#include "connections.hpp"

#include <cstdlib>
#include <vector>

namespace pm {

namespace {

std::set<std::string> keys_of(const Json& def, const char* key) {
    std::set<std::string> out;
    if (def.is_object() && def.contains(key) && def[key].is_object())
        for (auto it = def[key].begin(); it != def[key].end(); ++it) out.insert(it.key());
    return out;
}

std::set<std::string> list_of(const Json& u, const char* key) {
    std::set<std::string> out;
    if (u.is_object() && u.contains(key) && u[key].is_array())
        for (const auto& x : u[key])
            if (x.is_string()) out.insert(x.get<std::string>());
    return out;
}

bool parse_version(const std::string& v, long& major, long& minor) {
    const size_t dot = v.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= v.size()) return false;
    char* end = nullptr;
    major = std::strtol(v.c_str(), &end, 10);
    if (end != v.c_str() + dot) return false;
    minor = std::strtol(v.c_str() + dot + 1, &end, 10);
    return *end == '\0';
}

std::string join(const std::set<std::string>& s) {
    std::string out;
    for (const auto& x : s) out += (out.empty() ? "" : ", ") + x;
    return out;
}

}  // namespace

WorkerIo worker_io_from(const Json& io, const Json& config) {
    WorkerIo w;
    if (config.is_object() && config.contains("inputs") && config["inputs"].is_object()) w.config_inputs = config["inputs"];
    if (!io.is_object()) return w;
    if (io.contains("uses") && io["uses"].is_object())
        for (const auto& [input, u] : io["uses"].items()) {
            UsesEntry e;
            e.role = u.value("from", std::string());
            e.version = u.value("version", std::string());
            e.topics = list_of(u, "topics");
            e.commands = list_of(u, "commands");
            e.data = list_of(u, "data");
            e.required = u.value("required", false);
            w.uses[input] = e;
        }
    if (io.contains("provides") && io["provides"].is_object())
        for (const auto& [role, def] : io["provides"].items()) {
            ProvidedRole p;
            p.version = def.is_object() ? def.value("version", std::string()) : "";
            p.topics = keys_of(def, "topics");
            p.commands = keys_of(def, "commands");
            p.data = keys_of(def, "data");
            w.provides[role] = p;
        }
    return w;
}

bool version_compatible(const std::string& uses, const std::string& provided) {
    long um = 0, un = 0, pm_ = 0, pn = 0;
    if (!parse_version(uses, um, un) || !parse_version(provided, pm_, pn)) return false;
    return um == pm_ && pn >= un;
}

std::string connections_shape_error(const Json& connections) {
    if (connections.is_null()) return "";
    if (!connections.is_array()) return "connections must be a list [{\"to\": \"<worker>.<input>\", \"from\": \"<worker>\"}]";
    for (const auto& c : connections) {
        if (!c.is_object() || !c.contains("to") || !c["to"].is_string() || !c.contains("from") || !c["from"].is_string())
            return "connections items must be {\"to\": \"<worker>.<input>\", \"from\": \"<worker>\"}";
        const std::string to = c["to"].get<std::string>();
        const size_t dot = to.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= to.size())
            return "connections: to " + py_quote(to) + " must be <worker>.<input>";
    }
    return "";
}

InputResolution resolve_connections(const Json& connections, const std::map<std::string, WorkerIo>& workers) {
    InputResolution r;
    // 선언: "<워커>.<입력>" → 상대
    std::map<std::string, std::string> declared;
    if (connections.is_array())
        for (const auto& c : connections) {
            if (!c.is_object() || !c.contains("to") || !c["to"].is_string() || !c.contains("from") || !c["from"].is_string()) continue;
            const std::string to = c["to"].get<std::string>(), from = c["from"].get<std::string>();
            const size_t dot = to.find('.');
            const std::string w = to.substr(0, dot), input = dot == std::string::npos ? "" : to.substr(dot + 1);
            const auto wi = workers.find(w);
            if (wi == workers.end()) r.errors.push_back(to + ": no worker " + py_quote(w));
            else if (!wi->second.uses.count(input)) r.errors.push_back(to + ": " + w + " has no input " + py_quote(input) + " (io.json uses)");
            else if (declared.count(to)) r.errors.push_back(to + ": declared twice (" + declared[to] + ", " + from + ")");
            else declared[to] = from;
        }
    for (const auto& [id, w] : workers) {
        Json st = Json::object();
        Json inj = Json::object();
        for (const auto& [input, u] : w.uses) {
            Json e = Json::object();
            e["required"] = u.required;
            std::string from, how, error;
            const auto d = declared.find(id + "." + input);
            if (d != declared.end()) {
                from = d->second;
                how = "declared";
            } else {
                std::vector<std::string> providers;
                for (const auto& [pid, p] : workers)
                    if (pid != id && p.provides.count(u.role)) providers.push_back(pid);
                if (providers.size() == 1) {
                    from = providers[0];
                    how = "auto";
                } else if (w.config_inputs.contains(input)) {
                    how = "config";
                    const Json& c = w.config_inputs[input];
                    from = c.is_object() ? c.value("worker", std::string()) : "";
                } else if (providers.size() > 1) {
                    std::set<std::string> ps(providers.begin(), providers.end());
                    error = "several workers provide " + py_quote(u.role) + " (" + join(ps) + "): declare it in connections.json";
                }
            }
            // 검사(선언·자동): 프리셋에 적은 주소는 사람이 정한 것이라 그대로(상대가 워크스페이스 밖일 수 있음)
            if (how == "declared" || how == "auto") {
                const auto pi = workers.find(from);
                if (from == id) error = "a worker cannot use itself";
                else if (pi == workers.end()) error = "no worker " + py_quote(from);
                else if (!pi->second.provides.count(u.role)) error = from + " does not provide " + py_quote(u.role);
                else {
                    const ProvidedRole& p = pi->second.provides.at(u.role);
                    std::set<std::string> miss;
                    for (const auto& t : u.topics)
                        if (!p.topics.count(t)) miss.insert("topic " + t);
                    for (const auto& c : u.commands)
                        if (!p.commands.count(c)) miss.insert("command " + c);
                    for (const auto& x : u.data)
                        if (!p.data.count(x)) miss.insert("data " + x);
                    if (!version_compatible(u.version, p.version))
                        error = u.role + " " + u.version + " (copy) does not match " + from + "'s " + p.version;
                    else if (!miss.empty())
                        error = from + "'s " + u.role + " lacks " + join(miss);
                }
            }
            e["from"] = from.empty() ? Json(nullptr) : Json(from);
            e["how"] = how.empty() ? Json(nullptr) : Json(how);
            e["ok"] = error.empty() && (!how.empty() || !u.required);
            e["error"] = !error.empty() ? Json(error) : (how.empty() && u.required ? Json("required input not connected") : Json(nullptr));
            if (error.empty() && (how == "declared" || how == "auto")) inj[input] = Json{{"worker", from}};
            st[input] = e;
        }
        r.status[id] = st;
        r.inject[id] = inj;
    }
    return r;
}

}  // namespace pm
