#include "peers.hpp"

#include <regex>

namespace pm {

namespace {

const std::regex kId("^[a-z0-9][a-z0-9_-]{0,63}$");

void add_commands(const Json& def, std::set<std::string>* all, std::set<std::string>* read,
                  std::set<std::string>* operate = nullptr) {
    if (!def.is_object()) return;
    auto it = def.find("commands");
    if (it == def.end() || !it->is_object()) return;
    for (auto c = it->begin(); c != it->end(); ++c) {
        if (all) all->insert(c.key());
        if (read && c->is_object() && c->value("read", false) == true) read->insert(c.key());
        if (operate && c->is_object() && c->value("operate", false) == true) operate->insert(c.key());
    }
}

// "<워커>[.<역할>]" → (워커, 역할). 역할이 없으면 ""
std::pair<std::string, std::string> split_key(const std::string& key) {
    const size_t dot = key.find('.');
    if (dot == std::string::npos) return {key, ""};
    return {key.substr(0, dot), key.substr(dot + 1)};
}

}  // namespace

WorkerCommands commands_from_io(const Json& io) {
    WorkerCommands w;
    w.read.insert("get_status");
    if (!io.is_object()) return w;
    for (const char* k : {"common", "self"}) add_commands(io.value(k, Json()), nullptr, &w.read, &w.operate);
    if (io.contains("includes") && io["includes"].is_object())
        for (const auto& [path, def] : io["includes"].items()) add_commands(def, nullptr, &w.read, &w.operate);
    if (io.contains("provides") && io["provides"].is_object())
        for (const auto& [role, def] : io["provides"].items()) add_commands(def, &w.roles[role], &w.read, &w.operate);
    if (io.contains("uses") && io["uses"].is_object())
        for (const auto& [input, u] : io["uses"].items())
            if (u.is_object() && u.contains("from") && u["from"].is_string()) w.uses.insert(u["from"].get<std::string>());
    return w;
}

std::string commanders_shape_error(const Json& commanders) {
    if (commanders.is_null()) return "";
    if (!commanders.is_object()) return "commanders must be an object {\"<worker>[.<role>]\": [\"<id>\", ...]}";
    for (const auto& [key, list] : commanders.items()) {
        const auto [w, r] = split_key(key);
        if (!std::regex_match(w, kId) || (key.find('.') != std::string::npos && !std::regex_match(r, kId)))
            return "commanders key " + py_quote(key) + ": must be <worker> or <worker>.<role>";
        if (!list.is_array()) return "commanders." + key + " must be a list of ids";
        for (const auto& c : list)
            if (!c.is_string() || !std::regex_match(c.get<std::string>(), kId))
                return "commanders." + key + ": ids must be strings like worker ids";
    }
    return "";
}

Json check_commanders(const Json& commanders, const std::map<std::string, WorkerCommands>& workers) {
    Json out = Json::object();
    if (!commanders.is_object()) return out;
    for (const auto& [key, list] : commanders.items()) {
        Json e = Json::object();
        e["ok"] = true;
        e["error"] = nullptr;
        Json warnings = Json::array();
        const auto [w, role] = split_key(key);
        const auto wi = workers.find(w);
        if (wi == workers.end()) {
            e["ok"] = false;
            e["error"] = "no worker " + py_quote(w);
        } else if (!role.empty() && !wi->second.roles.count(role)) {
            e["ok"] = false;
            e["error"] = w + " does not provide " + py_quote(role) + " (io.json provides)";
        } else if (role.empty() && wi->second.roles.empty()) {
            e["ok"] = false;
            e["error"] = w + " has no role definitions (io.json provides)";
        }
        if (list.is_array())
            for (const auto& c : list) {
                if (!c.is_string()) continue;
                const std::string id = c.get<std::string>();
                if (id == w) {
                    warnings.push_back(id + ": a worker does not command itself");
                    continue;
                }
                const auto ci = workers.find(id);
                if (ci == workers.end()) {
                    warnings.push_back(id + ": not a worker on this board");
                    continue;
                }
                if (wi == workers.end()) continue;
                std::set<std::string> roles;
                if (!role.empty()) roles.insert(role);
                else
                    for (const auto& [r, cmds] : wi->second.roles) roles.insert(r);
                for (const auto& r : roles)
                    if (!ci->second.uses.count(r)) warnings.push_back(id + ": does not declare uses of " + py_quote(r) + " (io.json)");
            }
        e["warnings"] = warnings;
        out[key] = e;
    }
    return out;
}

Json build_peers(const Json& commanders, const std::map<std::string, WorkerCommands>& workers,
                 const std::vector<PeerProcess>& procs, const std::string& mode, const std::string& guard,
                 const Json& boot_id) {
    Json out = Json::object();
    out["version"] = 1;
    out["guard"] = guard;
    out["mode"] = mode;
    out["boot_id"] = boot_id;
    Json peers = Json::object();
    for (const auto& p : procs) {
        if (p.id.empty() || (p.pid <= 0 && p.pgid <= 0)) continue;
        Json e = Json::object();
        if (p.pid > 0) e["pid"] = p.pid;
        if (p.pgid > 0) e["pgid"] = p.pgid;
        if (p.trusted_from) e["trusted_from"] = true;
        peers[p.id] = e;
    }
    out["peers"] = peers;
    const bool dev = mode == "development";
    Json ws = Json::object();
    for (const auto& [id, cmds] : workers) {
        Json w = Json::object();
        Json read = Json::array();
        for (const auto& c : cmds.read) read.push_back(c);
        w["read"] = read;
        // 명령하는 곳 → 허용 명령(집합으로 모은 뒤 배열로. "*"는 전부)
        std::map<std::string, std::set<std::string>> allow;
        std::set<std::string> star = {"gateway"};
        for (const auto& p : procs)  // 믿는 곳(게이트웨이처럼 넘겨받은 명령을 이미 검사한 곳: 명령 입구 매니저)은 전부
            if (p.trusted_from && !p.id.empty()) star.insert(p.id);
        if (commanders.is_object())
            for (const auto& [key, list] : commanders.items()) {
                const auto [wk, role] = split_key(key);
                if (wk != id || !list.is_array()) continue;
                if (!role.empty() && !cmds.roles.count(role)) continue;  // 검사 오류인 항목은 아무것도 열지 않음
                if (role.empty() && cmds.roles.empty()) continue;
                for (const auto& c : list) {
                    if (!c.is_string() || c.get<std::string>() == id) continue;
                    const std::string who = c.get<std::string>();
                    if (dev) {
                        star.insert(who);
                        continue;
                    }
                    auto& set = allow[who];
                    for (const auto& [r, rc] : cmds.roles)
                        if (role.empty() || r == role) set.insert(rc.begin(), rc.end());
                    if (role.empty()) set.insert(cmds.operate.begin(), cmds.operate.end());  // 워커 전체: 운용 조작도
                }
            }
        Json a = Json::object();  // 순서 있는 객체: 게이트웨이를 먼저, 나머지는 이름순
        a["gateway"] = "*";
        for (const auto& who : star) a[who] = "*";
        for (const auto& [who, set] : allow) {
            if (star.count(who)) continue;
            Json arr = Json::array();
            for (const auto& c : set) arr.push_back(c);
            a[who] = arr;
        }
        w["allow"] = a;
        ws[id] = w;
    }
    out["workers"] = ws;
    return out;
}

}  // namespace pm
