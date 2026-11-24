#include "mission/dispatch.hpp"

#include <stdexcept>

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

int PriorityRules::priority_of(const std::string& origin) const {
    if (auto it = origins.find(origin); it != origins.end()) return it->second;
    size_t best = 0;
    int p = dflt;
    for (const auto& [k, v] : origins)  // "remote:*": 앞부분이 가장 긴 것
        if (!k.empty() && k.back() == '*' && origin.compare(0, k.size() - 1, k, 0, k.size() - 1) == 0 && k.size() > best) {
            best = k.size();
            p = v;
        }
    return p;
}

int64_t PriorityRules::hold_of(const std::string& role) const {
    auto it = role_hold_ns.find(role);
    return it != role_hold_ns.end() ? it->second : hold_ns;
}

PriorityRules priority_from_config(const Json& j) {
    PriorityRules p;
    if (j.is_null()) return p;
    if (!j.is_object()) throw std::runtime_error("mission.priority must be an object");
    for (const auto& [k, v] : j.items())
        if (k != "default" && k != "origins" && k != "hold_ms" && k != "roles")
            throw std::runtime_error("mission.priority: unknown key " + k);
    auto int_of = [](const Json& v, const std::string& what, int lo, int hi) {
        if (!v.is_number_integer() || v.get<int64_t>() < lo || v.get<int64_t>() > hi)
            throw std::runtime_error(what + " must be an integer " + std::to_string(lo) + ".." + std::to_string(hi));
        return v.get<int>();
    };
    if (j.contains("default")) p.dflt = int_of(j["default"], "mission.priority.default", 0, 1000);
    if (j.contains("origins")) {
        if (!j["origins"].is_object()) throw std::runtime_error("mission.priority.origins must be an object");
        for (const auto& [k, v] : j["origins"].items()) p.origins[k] = int_of(v, "mission.priority.origins." + k, 0, 1000);
    }
    if (j.contains("hold_ms")) p.hold_ns = int64_t(int_of(j["hold_ms"], "mission.priority.hold_ms", 0, 600000)) * 1000000;
    if (j.contains("roles")) {
        if (!j["roles"].is_object()) throw std::runtime_error("mission.priority.roles must be an object");
        for (const auto& [role, v] : j["roles"].items()) {
            if (!v.is_object() || !v.contains("hold_ms") || v.size() != 1)
                throw std::runtime_error("mission.priority.roles." + role + " must be {\"hold_ms\": n}");
            p.role_hold_ns[role] = int64_t(int_of(v["hold_ms"], "mission.priority.roles." + role + ".hold_ms", 0, 600000)) * 1000000;
        }
    }
    return p;
}

std::vector<Condition> conditions_from_config(const Json& j) {
    std::vector<Condition> out;
    if (j.is_null()) return out;
    if (!j.is_array()) throw std::runtime_error("mission.conditions must be an array");
    static const std::set<std::string> ops = {"==", "!=", "<", "<=", ">", ">="};
    for (size_t i = 0; i < j.size(); ++i) {
        const Json& c = j[i];
        const std::string where = "mission.conditions[" + std::to_string(i) + "]";
        if (!c.is_object()) throw std::runtime_error(where + " must be an object");
        for (const auto& [k, v] : c.items())
            if (k != "name" && k != "target" && k != "cmd" && k != "require" && k != "except" && k != "unknown" && k != "message")
                throw std::runtime_error(where + ": unknown key " + k);
        Condition x;
        x.name = c.value("name", "rule" + std::to_string(i + 1));
        x.target = c.value("target", std::string("*"));
        x.cmd = c.value("cmd", std::string("*"));
        x.message = c.value("message", std::string());
        const std::string unk = c.value("unknown", std::string("block"));
        if (unk != "block" && unk != "allow") throw std::runtime_error(where + ".unknown must be block or allow");
        x.unknown_allow = unk == "allow";
        if (c.contains("except")) {
            if (!c["except"].is_array()) throw std::runtime_error(where + ".except must be an array");
            for (const auto& e : c["except"]) x.except.insert(e.get<std::string>());
        }
        if (!c.contains("require") || !c["require"].is_array() || c["require"].empty())
            throw std::runtime_error(where + ".require must be a non-empty array");
        for (const auto& n : c["require"]) {
            if (!n.is_object()) throw std::runtime_error(where + ".require items must be objects");
            Condition::Need need;
            need.worker = n.value("worker", std::string());
            if (need.worker == "$target") need.worker.clear();
            if (n.contains("metric")) {
                need.kind = "metric";
                need.key = n["metric"].get<std::string>();
                need.op = n.value("op", std::string("=="));
                if (!ops.count(need.op)) throw std::runtime_error(where + ": op must be one of == != < <= > >=");
                if (!n.contains("value") || !n["value"].is_number()) throw std::runtime_error(where + ": metric needs a numeric value");
                need.value = n["value"];
            } else if (n.contains("state")) {
                need.kind = "state";
                need.key = n["state"].get<std::string>();
            } else if (n.contains("mode")) {
                need.kind = "mode";
                need.key = n["mode"].get<std::string>();
            } else {
                throw std::runtime_error(where + ".require items need metric, state or mode");
            }
            x.require.push_back(need);
        }
        out.push_back(x);
    }
    return out;
}

static bool glob_match(const std::string& pat, const std::string& s) {
    if (pat == "*") return true;
    if (!pat.empty() && pat.back() == '*') return s.compare(0, pat.size() - 1, pat, 0, pat.size() - 1) == 0;
    return pat == s;
}

std::optional<std::string> check_conditions(const std::vector<Condition>& cs, const World& w, const Request& r) {
    for (const auto& c : cs) {
        if (!glob_match(c.target, r.target) || !glob_match(c.cmd, r.cmd) || c.except.count(r.origin)) continue;
        for (const auto& n : c.require) {
            const std::string wk = n.worker.empty() ? r.target : n.worker;
            std::string have;
            bool known = true, ok = false;
            if (n.kind == "mode") {
                known = !w.mode.empty();
                have = w.mode;
                ok = have == n.key;
            } else if (n.kind == "state") {
                auto it = w.state.find(wk);
                known = it != w.state.end();
                have = known ? it->second : "";
                ok = have == n.key;
            } else {
                auto it = w.metrics.find(wk);
                known = it != w.metrics.end() && it->second.is_object() && it->second.contains(n.key) && it->second[n.key].is_number();
                if (known) {
                    const double v = it->second[n.key].get<double>(), x = n.value.get<double>();
                    have = it->second[n.key].dump();
                    ok = n.op == "==" ? v == x : n.op == "!=" ? v != x : n.op == "<" ? v < x : n.op == "<=" ? v <= x : n.op == ">" ? v > x : v >= x;
                }
            }
            if (!known && c.unknown_allow) continue;
            if (known && ok) continue;
            std::string what = n.kind == "mode" ? "mode" : wk + (n.kind == "state" ? " state" : "." + n.key);
            std::string need = n.kind == "metric" ? n.op + " " + n.value.dump() : "== " + n.key;
            std::string why = "condition " + c.name + ": " + what + " is " + (known ? have : "unknown") + " (need " + need + ")";
            if (!c.message.empty()) why += ": " + c.message;
            return why;
        }
    }
    return std::nullopt;
}

Dispatcher::Admit Dispatcher::submit(const Request& r) {
    last_now_ = r.enq_ns;
    Lane& l = lanes_[r.key()];
    if (held_against(l, r, r.enq_ns)) return Admit::Held;
    if (!l.inflight) {
        l.inflight = r;
        l.inflight_since = r.enq_ns;
        return Admit::Send;
    }
    if (l.queue.size() >= max_) {
        // 가장 낮은 우선순위 중 가장 늦게 온 것보다 높으면 그것을 밀어냄
        if (l.queue.empty() || l.queue.back().priority >= r.priority) return Admit::Busy;
        refused_.push_back({l.queue.back(), "preempted by " + r.origin + " (priority " + std::to_string(r.priority) + ")"});
        l.queue.pop_back();
    }
    auto pos = l.queue.begin();
    while (pos != l.queue.end() && pos->priority >= r.priority) ++pos;
    l.queue.insert(pos, r);
    return Admit::Queued;
}

std::optional<Request> Dispatcher::done(const std::string& key, int64_t now_ns) {
    auto it = lanes_.find(key);
    if (it == lanes_.end()) return std::nullopt;
    Lane& l = it->second;
    const int64_t now = now_ns >= 0 ? now_ns : last_now_;
    if (now_ns >= 0) last_now_ = now_ns;
    if (l.inflight) {
        const int64_t hold = rules_.hold_of(l.inflight->role);
        if (hold > 0 && (l.held_until <= now || l.inflight->priority >= l.holder_prio)) {
            l.holder = l.inflight->origin;
            l.holder_prio = l.inflight->priority;
            l.held_until = now + hold;
        }
    }
    l.inflight.reset();
    // 잡아 둔 동안 큐의 더 낮은 다른 곳 명령은 내보냄(held)
    for (auto q = l.queue.begin(); q != l.queue.end();) {
        if (held_against(l, *q, now)) {
            refused_.push_back({*q, key + " held by " + l.holder + " (priority " + std::to_string(l.holder_prio) + ")"});
            q = l.queue.erase(q);
        } else {
            ++q;
        }
    }
    if (l.queue.empty()) return std::nullopt;
    l.inflight = l.queue.front();
    l.queue.pop_front();
    l.inflight_since = now;
    return l.inflight;
}

std::vector<std::pair<Request, std::string>> Dispatcher::refused() {
    std::vector<std::pair<Request, std::string>> out;
    out.swap(refused_);
    return out;
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
    return it == lanes_.end() || (!it->second.inflight && it->second.queue.empty() && it->second.held_until <= last_now_);
}

size_t Dispatcher::waiting() const {
    size_t n = 0;
    for (const auto& [k, l] : lanes_) n += l.queue.size();
    return n;
}

Json Dispatcher::snapshot(int64_t now_ns) const {
    Json q = Json::object();
    for (const auto& [k, l] : lanes_) {
        const bool held = l.held_until > now_ns;
        if (!l.inflight && l.queue.empty() && !held) continue;
        Json e = Json::object();
        e["waiting"] = l.queue.size();
        e["inflight"] = l.inflight ? Json{{"cmd", l.inflight->cmd}, {"origin", l.inflight->origin},
                                          {"age_ms", (now_ns - l.inflight_since) / 1000000}}
                                   : Json(nullptr);
        if (held) e["holder"] = Json{{"origin", l.holder}, {"priority", l.holder_prio}, {"left_ms", (l.held_until - now_ns) / 1000000}};
        q[k] = e;
    }
    return q;
}

}  // namespace mission
