#include "payload/modules.hpp"

#include <fnmatch.h>

#include <stdexcept>

namespace payload {

namespace {
bool glob(const std::string& p, const std::string& s) { return fnmatch(p.c_str(), s.c_str(), 0) == 0; }
std::string str(const Json& o, const char* k) {
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}
}  // namespace

Groups parse_groups(const Json& g) {
    Groups out;
    if (g.is_null()) return out;
    if (!g.is_object()) throw std::invalid_argument("payload.groups must be {\"<name>\": [\"<worker id glob>\", ...]}");
    for (auto it = g.begin(); it != g.end(); ++it) {
        if (it.key().empty()) throw std::invalid_argument("payload.groups: empty group name");
        if (!it->is_array()) throw std::invalid_argument("payload.groups." + it.key() + " must be a list");
        std::vector<std::string> globs;
        for (const auto& x : *it) {
            if (!x.is_string()) throw std::invalid_argument("payload.groups." + it.key() + " items must be strings");
            globs.push_back(x.get<std::string>());
        }
        out[it.key()] = globs;
    }
    return out;
}

void Modules::on_worker(const Json& data) {
    const std::string id = str(data, "id");
    if (id.empty()) return;
    workers_[id].data = data;
}

void Modules::on_removed(const std::string& id) { workers_.erase(id); }

void Modules::set_roles(const std::string& id, std::vector<std::string> roles) {
    auto it = workers_.find(id);
    if (it != workers_.end()) it->second.roles = std::move(roles);
}

void Modules::set_health(const Json& system) {
    health_ = system.is_object() && system.contains("workers") && system["workers"].is_object() ? system["workers"] : Json();
}

bool Modules::is_manager(const std::string& id) const {
    auto it = workers_.find(id);
    return it != workers_.end() && str(it->second.data, "kind") == "manager";
}

std::vector<std::string> Modules::members(const std::string& group) const {
    std::vector<std::string> out;
    auto g = groups_.find(group);
    if (g == groups_.end()) return out;
    for (const auto& [id, e] : workers_) {
        if (str(e.data, "kind") == "manager") continue;
        for (const auto& p : g->second)
            if (glob(p, id)) {
                out.push_back(id);
                break;
            }
    }
    return out;
}

std::vector<std::string> Modules::ids() const {
    std::vector<std::string> out;
    for (const auto& [id, e] : workers_) out.push_back(id);
    return out;
}

size_t Modules::count() const {
    size_t n = 0;
    for (const auto& [id, e] : workers_) n += str(e.data, "kind") == "manager" ? 0 : 1;
    return n;
}

size_t Modules::running() const {
    size_t n = 0;
    for (const auto& [id, e] : workers_)
        if (str(e.data, "kind") != "manager" && str(e.data, "actual_state") == "running") ++n;
    return n;
}

Json Modules::snapshot() const {
    Json mods = Json::object();
    for (const auto& [id, e] : workers_) {
        if (str(e.data, "kind") == "manager") continue;
        Json m = Json::object();
        m["roles"] = e.roles;
        m["state"] = str(e.data, "actual_state");
        m["desired"] = str(e.data, "desired_state");
        m["autostart"] = e.data.value("autostart", false);
        const Json h = health_.is_object() && health_.contains(id) ? health_[id] : Json();
        m["health"] = h.is_object() ? h.value("health", Json()) : Json(nullptr);
        m["why"] = h.is_object() ? h.value("why", Json()) : Json(nullptr);
        Json gs = Json::array();
        for (const auto& [name, globs] : groups_)
            for (const auto& p : globs)
                if (glob(p, id)) {
                    gs.push_back(name);
                    break;
                }
        m["groups"] = gs;
        mods[id] = m;
    }
    Json groups = Json::object();
    for (const auto& [name, globs] : groups_) groups[name] = members(name);
    return {{"modules", mods}, {"groups", groups}};
}

}  // namespace payload
