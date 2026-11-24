#include "fault/engine.hpp"

#include <fnmatch.h>

#include <stdexcept>

namespace fault {

namespace {

bool glob(const std::string& p, const std::string& s) { return fnmatch(p.c_str(), s.c_str(), 0) == 0; }

std::set<std::string> string_set(const Json& j, const std::string& where) {
    std::set<std::string> out;
    if (j.is_string()) {
        out.insert(j.get<std::string>());
        return out;
    }
    if (!j.is_array()) throw std::invalid_argument(where + " must be a string or a list");
    for (const auto& x : j) {
        if (!x.is_string()) throw std::invalid_argument(where + " items must be strings");
        out.insert(x.get<std::string>());
    }
    return out;
}

void only_keys(const Json& o, std::initializer_list<const char*> keys, const std::string& where) {
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool ok = false;
        for (const char* k : keys) ok |= it.key() == k;
        if (!ok) throw std::invalid_argument(where + ": unknown key " + it.key());
    }
}

const int64_t kStatusLostNs = 3LL * 1000000000;

}  // namespace

int severity_rank(const std::string& s) {
    if (s == "warning") return 1;
    if (s == "error") return 2;
    if (s == "critical") return 3;
    return 0;
}

std::vector<Rule> parse_rules(const Json& rules) {
    std::vector<Rule> out;
    if (rules.is_null()) return out;
    if (!rules.is_array()) throw std::invalid_argument("fault.rules must be a list");
    std::set<std::string> ids;
    for (const auto& r : rules) {
        if (!r.is_object() || !r.contains("id") || !r["id"].is_string() || r["id"].get<std::string>().empty())
            throw std::invalid_argument("fault.rules items need \"id\"");
        Rule rule;
        rule.id = r["id"].get<std::string>();
        const std::string w = "fault.rules." + rule.id;
        if (!ids.insert(rule.id).second) throw std::invalid_argument(w + ": id used twice");
        only_keys(r, {"id", "when", "for_s", "hold_s", "severity", "text", "do"}, w);
        if (!r.contains("when") || !r["when"].is_object() || r["when"].size() != 1)
            throw std::invalid_argument(w + ".when must have exactly one of health, metric, event, system");
        const auto wit = r["when"].begin();
        const Json& c = wit.value();
        if (!c.is_object()) throw std::invalid_argument(w + ".when." + wit.key() + " must be an object");
        Condition& cond = rule.when;
        if (wit.key() == "health") {
            cond.kind = Condition::Health;
            only_keys(c, {"worker", "is"}, w + ".when.health");
            if (!c.contains("is")) throw std::invalid_argument(w + ".when.health needs \"is\"");
            cond.is = string_set(c["is"], w + ".when.health.is");
        } else if (wit.key() == "metric") {
            cond.kind = Condition::Metric;
            only_keys(c, {"worker", "name", "above", "below"}, w + ".when.metric");
            if (!c.contains("name") || !c["name"].is_string()) throw std::invalid_argument(w + ".when.metric needs \"name\"");
            cond.name = c["name"].get<std::string>();
            if (c.contains("above")) {
                if (!c["above"].is_number()) throw std::invalid_argument(w + ".when.metric.above must be a number");
                cond.above = c["above"].get<double>();
            }
            if (c.contains("below")) {
                if (!c["below"].is_number()) throw std::invalid_argument(w + ".when.metric.below must be a number");
                cond.below = c["below"].get<double>();
            }
            if (!cond.above && !cond.below) throw std::invalid_argument(w + ".when.metric needs above or below");
        } else if (wit.key() == "event") {
            cond.kind = Condition::Event;
            only_keys(c, {"worker", "code"}, w + ".when.event");
            if (!c.contains("code") || !c["code"].is_string()) throw std::invalid_argument(w + ".when.event needs \"code\"");
            cond.code = c["code"].get<std::string>();
        } else if (wit.key() == "system") {
            cond.kind = Condition::System;
            only_keys(c, {"is"}, w + ".when.system");
            if (!c.contains("is")) throw std::invalid_argument(w + ".when.system needs \"is\"");
            cond.is = string_set(c["is"], w + ".when.system.is");
        } else {
            throw std::invalid_argument(w + ".when: unknown condition " + wit.key());
        }
        if (c.contains("worker")) {
            if (!c["worker"].is_string()) throw std::invalid_argument(w + ".when worker must be a glob string");
            cond.worker = c["worker"].get<std::string>();
        }
        auto num = [&](const char* k, double& out) {
            if (!r.contains(k)) return;
            if (!r[k].is_number() || r[k].get<double>() < 0) throw std::invalid_argument(w + "." + k + " must be a number >= 0");
            out = r[k].get<double>();
        };
        num("for_s", rule.for_s);
        num("hold_s", rule.hold_s);
        if (r.contains("severity")) {
            if (!r["severity"].is_string() || severity_rank(r["severity"].get<std::string>()) == 0)
                throw std::invalid_argument(w + ".severity must be warning, error or critical");
            rule.severity = r["severity"].get<std::string>();
        }
        rule.text = r.contains("text") && r["text"].is_string() ? r["text"].get<std::string>() : rule.id;
        if (r.contains("do")) {
            if (!r["do"].is_array()) throw std::invalid_argument(w + ".do must be a list");
            for (const auto& a : r["do"]) {
                if (!a.is_object() || !a.contains("target") || !a["target"].is_string() || !a.contains("cmd") || !a["cmd"].is_string())
                    throw std::invalid_argument(w + ".do items need \"target\" and \"cmd\"");
                only_keys(a, {"target", "cmd", "args"}, w + ".do");
                Action act;
                act.target = a["target"].get<std::string>();
                act.cmd = a["cmd"].get<std::string>();
                if (a.contains("args")) {
                    if (!a["args"].is_object()) throw std::invalid_argument(w + ".do args must be an object");
                    act.args = a["args"];
                }
                rule.actions.push_back(act);
            }
        }
        out.push_back(rule);
    }
    return out;
}

void Engine::set_system(const Json& system, int64_t now_ns) {
    if (system.is_object()) {
        health_ = system.contains("workers") ? system["workers"] : Json::object();
        system_health_ = system.value("health", "");
        status_link_ = true;
        status_lost_since_ = 0;
    } else {
        if (status_link_ || status_lost_since_ == 0) status_lost_since_ = now_ns;
        status_link_ = false;
    }
}

void Engine::set_metrics(const std::string& worker, const Json& metrics) { metrics_[worker] = metrics; }

void Engine::remove_worker(const std::string& worker) { metrics_.erase(worker); }

void Engine::on_event(const std::string& src, const Json& event, int64_t now_ns) {
    if (!event.is_object()) return;
    const std::string code = event.value("code", "");
    for (const auto& r : rules_)
        if (r.when.kind == Condition::Event && glob(r.when.worker, src) && glob(r.when.code, code))
            event_at_[r.id + ":" + src] = now_ns;
}

void Engine::raise(const std::string& key, const Rule& r, const std::string& worker, int64_t now, bool operation, Out& out) {
    Active a;
    a.rule = r.id;
    a.worker = worker;
    a.severity = r.severity;
    a.text = r.text + (worker.empty() ? "" : " (" + worker + ")");
    a.since = now;
    for (size_t i = 0; i < r.actions.size(); ++i) {
        const Action& act = r.actions[i];
        Json e = {{"target", act.target}, {"cmd", act.cmd}, {"ok", nullptr}};
        if (!operation) e["skipped"] = "development mode";
        else out.sends.push_back({key, i, act});
        a.actions.push_back(e);
    }
    out.events.push_back({{"severity", r.severity}, {"code", "fault_raised"}, {"text", a.text},
                          {"detail", {{"fault", key}, {"rule", r.id}, {"worker", worker.empty() ? Json(nullptr) : Json(worker)}}}});
    active_[key] = std::move(a);
}

void Engine::clear(const std::string& key, Out& out) {
    auto it = active_.find(key);
    if (it == active_.end()) return;
    out.events.push_back({{"severity", "info"}, {"code", "fault_cleared"}, {"text", it->second.text + " cleared"},
                          {"detail", {{"fault", key}, {"rule", it->second.rule}}}});
    active_.erase(it);
}

Engine::Out Engine::tick(int64_t now_ns, bool operation) {
    Out out;
    std::set<std::string> seen;  // 이번에 조건을 본 이상(그 밖의 올라간 이상은 풀린 것)
    auto consider = [&](const Rule& r, const std::string& worker, bool cond) {
        const std::string key = worker.empty() ? r.id : r.id + ":" + worker;
        seen.insert(key);
        if (!cond) {
            true_since_.erase(key);
            clear(key, out);
            return;
        }
        auto ts = true_since_.find(key);
        if (ts == true_since_.end()) ts = true_since_.emplace(key, now_ns).first;
        if (!active_.count(key) && now_ns - ts->second >= static_cast<int64_t>(r.for_s * 1e9)) raise(key, r, worker, now_ns, operation, out);
    };
    const bool status_ok = status_link_;
    for (const auto& r : rules_) {
        switch (r.when.kind) {
        case Condition::Health:
            if (!status_ok) {  // 판정할 수 없음: 올라간 이상은 그대로 둠
                for (const auto& [k, a] : active_)
                    if (a.rule == r.id) seen.insert(k);
                break;
            }
            if (health_.is_object())
                for (auto it = health_.begin(); it != health_.end(); ++it)
                    if (glob(r.when.worker, it.key()))
                        consider(r, it.key(), it->is_object() && r.when.is.count(it->value("health", "")) > 0);
            break;
        case Condition::Metric:
            for (const auto& [w, m] : metrics_) {
                if (!glob(r.when.worker, w) || !m.is_object() || !m.contains(r.when.name) || !m[r.when.name].is_number()) continue;
                const double v = m[r.when.name].get<double>();
                consider(r, w, (r.when.above && v > *r.when.above) || (r.when.below && v < *r.when.below));
            }
            break;
        case Condition::Event:
            for (auto it = event_at_.begin(); it != event_at_.end();) {
                if (it->first.rfind(r.id + ":", 0) != 0) {
                    ++it;
                    continue;
                }
                const std::string w = it->first.substr(r.id.size() + 1);
                const bool on = now_ns - it->second < static_cast<int64_t>(r.hold_s * 1e9);
                consider(r, w, on);
                it = on ? std::next(it) : event_at_.erase(it);
            }
            break;
        case Condition::System:
            if (!status_ok) {
                if (active_.count(r.id)) seen.insert(r.id);
                break;
            }
            consider(r, "", r.when.is.count(system_health_) > 0);
            break;
        }
    }
    // status 끊김(스스로 올리는 이상)
    static const Rule lost{"status_lost", "warning", "status-manager lost", {}, 0, 0, {}};
    const bool lost_now = !status_link_ && status_lost_since_ != 0 && now_ns - status_lost_since_ >= kStatusLostNs;
    seen.insert("status_lost");
    if (lost_now && !active_.count("status_lost")) raise("status_lost", lost, "", now_ns, operation, out);
    if (!lost_now) clear("status_lost", out);
    // 조건을 더 볼 수 없는 이상(워커가 사라짐 등)은 내림
    std::vector<std::string> gone;
    for (const auto& [k, a] : active_)
        if (!seen.count(k)) gone.push_back(k);
    for (const auto& k : gone) {
        true_since_.erase(k);
        clear(k, out);
    }
    return out;
}

void Engine::action_result(const std::string& fault, size_t index, bool ok, const Json& error) {
    auto it = active_.find(fault);
    if (it == active_.end() || index >= it->second.actions.size()) return;
    it->second.actions[index]["ok"] = ok;
    if (!ok) it->second.actions[index]["error"] = error;
}

bool Engine::ack(const std::string& fault) {
    auto it = active_.find(fault);
    if (it == active_.end()) return false;
    it->second.acked = true;
    return true;
}

std::string Engine::worst() const {
    int w = 0;
    std::string s = "none";
    for (const auto& [k, a] : active_)
        if (severity_rank(a.severity) > w) {
            w = severity_rank(a.severity);
            s = a.severity;
        }
    return s;
}

Json Engine::snapshot(int64_t now_ns) const {
    Json act = Json::object();
    Json counts = {{"warning", 0}, {"error", 0}, {"critical", 0}};
    for (const auto& [k, a] : active_) {
        act[k] = {{"rule", a.rule}, {"worker", a.worker.empty() ? Json(nullptr) : Json(a.worker)}, {"severity", a.severity},
                  {"text", a.text}, {"since_ns", a.since}, {"age_ms", (now_ns - a.since) / 1000000}, {"acked", a.acked},
                  {"actions", a.actions}};
        counts[a.severity] = counts[a.severity].get<int>() + 1;
    }
    return {{"active", act}, {"counts", counts}, {"worst", worst()}};
}

}  // namespace fault
