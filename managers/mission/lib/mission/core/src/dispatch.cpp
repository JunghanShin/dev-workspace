#include "mission/dispatch.hpp"

namespace mission {

Access access_from_peers(const Json& doc) {
    Access a;
    if (!doc.is_object() || !doc.contains("workers") || !doc["workers"].is_object()) return a;
    for (const auto& [target, w] : doc["workers"].items()) {
        if (!w.is_object() || !w.contains("via") || !w["via"].is_object()) continue;
        for (const auto& [who, v] : w["via"].items()) {
            a.listed.insert(who);
            if (v == Json("*")) {
                a.via[target][who] = std::nullopt;
            } else if (v.is_array()) {
                std::set<std::string> s;
                for (const auto& c : v)
                    if (c.is_string()) s.insert(c.get<std::string>());
                a.via[target][who] = s;
            }
        }
    }
    return a;
}

Verdict check_access(const Access& a, const std::string& origin, const std::string& target, const std::string& cmd,
                     const Json& args, const Contract* c) {
    if (!a.listed.count(origin)) return Verdict::Pass;
    const auto t = a.via.find(target);
    if (t == a.via.end()) return Verdict::Deny;
    const auto w = t->second.find(origin);
    if (w == t->second.end()) return Verdict::Deny;
    if (!w->second) return Verdict::Allow;  // "*"
    if (w->second->count(cmd)) return Verdict::Allow;
    if (cmd == "lease.renew" && args.is_object() && args.value("role", Json()).is_string()) {
        const std::string role = args["role"].get<std::string>();
        for (const auto& x : *w->second)
            if (role_of(c, x) == role) return Verdict::Allow;
    }
    return Verdict::Deny;
}

bool fast_command(const Contract* c, const std::string& cmd) {
    return cmd == "get_status" || cmd == "lease.renew" || (c && c->read.count(cmd));
}

Contract contract_from_io(const Json& io) {
    Contract c;
    if (!io.is_object()) return c;
    auto commands_of = [](const Json& def) {
        std::vector<std::pair<std::string, bool>> out;
        if (def.is_object() && def.contains("commands") && def["commands"].is_object())
            for (auto it = def["commands"].begin(); it != def["commands"].end(); ++it)
                out.push_back({it.key(), it->is_object() && it->value("read", false)});
        return out;
    };
    if (io.contains("provides") && io["provides"].is_object())
        for (auto it = io["provides"].begin(); it != io["provides"].end(); ++it)
            for (const auto& [cmd, read] : commands_of(*it)) {
                c.cmd_role[cmd] = it.key();
                if (read) c.read.insert(cmd);
            }
    for (const char* k : {"self", "common"})
        if (io.contains(k))
            for (const auto& [cmd, read] : commands_of(io[k]))
                if (read) c.read.insert(cmd);
    if (io.contains("includes") && io["includes"].is_object())
        for (auto it = io["includes"].begin(); it != io["includes"].end(); ++it)
            for (const auto& [cmd, read] : commands_of(*it))
                if (read) c.read.insert(cmd);
    return c;
}

std::string role_of(const Contract* c, const std::string& cmd) {
    if (c) {
        auto it = c->cmd_role.find(cmd);
        return it != c->cmd_role.end() ? it->second : "self";
    }
    const size_t dot = cmd.find('.');
    return dot == std::string::npos ? "self" : cmd.substr(0, dot);
}

Dispatcher::Admit Dispatcher::submit(const Request& r) {
    last_now_ = r.enq_ns;
    Lane& l = lanes_[r.key()];
    if (!l.inflight) {
        l.inflight = r;
        l.inflight_since = r.enq_ns;
        return Admit::Send;
    }
    if (l.queue.size() >= max_) return Admit::Busy;
    l.queue.push_back(r);
    return Admit::Queued;
}

std::optional<Request> Dispatcher::done(const std::string& key) {
    auto it = lanes_.find(key);
    if (it == lanes_.end()) return std::nullopt;
    Lane& l = it->second;
    l.inflight.reset();
    if (l.queue.empty()) return std::nullopt;
    l.inflight = l.queue.front();
    l.queue.pop_front();
    l.inflight_since = last_now_;
    return l.inflight;
}

std::vector<Request> Dispatcher::expire(int64_t now_ns) {
    last_now_ = now_ns;
    std::vector<Request> out;
    for (auto& [k, l] : lanes_)
        while (!l.queue.empty() && now_ns - l.queue.front().enq_ns > total_) {
            out.push_back(l.queue.front());
            l.queue.pop_front();
        }
    return out;
}

bool Dispatcher::idle(const std::string& key) const {
    auto it = lanes_.find(key);
    return it == lanes_.end() || (!it->second.inflight && it->second.queue.empty());
}

size_t Dispatcher::waiting() const {
    size_t n = 0;
    for (const auto& [k, l] : lanes_) n += l.queue.size();
    return n;
}

Json Dispatcher::snapshot(int64_t now_ns) const {
    Json q = Json::object();
    for (const auto& [k, l] : lanes_) {
        if (!l.inflight && l.queue.empty()) continue;
        Json e = Json::object();
        e["waiting"] = l.queue.size();
        e["inflight"] = l.inflight ? Json{{"cmd", l.inflight->cmd}, {"origin", l.inflight->origin},
                                          {"age_ms", (now_ns - l.inflight_since) / 1000000}}
                                   : Json(nullptr);
        q[k] = e;
    }
    return q;
}

}  // namespace mission
