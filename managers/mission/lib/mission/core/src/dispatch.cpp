#include "mission/dispatch.hpp"

namespace mission {

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
