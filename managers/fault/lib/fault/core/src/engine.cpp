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

std::vector<Action> parse_actions(const Json& j, const std::string& w) {
    std::vector<Action> out;
    if (!j.is_array()) throw std::invalid_argument(w + " must be a list");
    for (const auto& a : j) {
        if (!a.is_object() || !a.contains("target") || !a["target"].is_string() || !a.contains("cmd") || !a["cmd"].is_string())
            throw std::invalid_argument(w + " items need \"target\" and \"cmd\"");
        only_keys(a, {"target", "cmd", "args"}, w);
        Action act;
        act.target = a["target"].get<std::string>();
        act.cmd = a["cmd"].get<std::string>();
        if (a.contains("args")) {
            if (!a["args"].is_object()) throw std::invalid_argument(w + " args must be an object");
            act.args = a["args"];
        }
        out.push_back(act);
    }
    return out;
}

Condition parse_condition(const Json& when, const std::string& w, int depth, int& leaves) {
    if (!when.is_object() || when.size() != 1)
        throw std::invalid_argument(w + " must have exactly one of health, metric, event, system, all, any");
    const auto wit = when.begin();
    const Json& c = wit.value();
    Condition cond;
    if (wit.key() == "all" || wit.key() == "any") {
        if (depth >= 4) throw std::invalid_argument(w + ": all/any nested too deep (max 4)");
        if (!c.is_array() || c.empty()) throw std::invalid_argument(w + "." + wit.key() + " must be a non-empty list");
        cond.kind = wit.key() == "all" ? Condition::All : Condition::Any;
        for (size_t i = 0; i < c.size(); ++i)
            cond.children.push_back(parse_condition(c[i], w + "." + wit.key() + "[" + std::to_string(i) + "]", depth + 1, leaves));
        return cond;
    }
    if (!c.is_object()) throw std::invalid_argument(w + "." + wit.key() + " must be an object");
    if (wit.key() == "health") {
        cond.kind = Condition::Health;
        only_keys(c, {"worker", "is"}, w + ".health");
        if (!c.contains("is")) throw std::invalid_argument(w + ".health needs \"is\"");
        cond.is = string_set(c["is"], w + ".health.is");
    } else if (wit.key() == "metric") {
        cond.kind = Condition::Metric;
        only_keys(c, {"worker", "name", "above", "below"}, w + ".metric");
        if (!c.contains("name") || !c["name"].is_string()) throw std::invalid_argument(w + ".metric needs \"name\"");
        cond.name = c["name"].get<std::string>();
        if (c.contains("above")) {
            if (!c["above"].is_number()) throw std::invalid_argument(w + ".metric.above must be a number");
            cond.above = c["above"].get<double>();
        }
        if (c.contains("below")) {
            if (!c["below"].is_number()) throw std::invalid_argument(w + ".metric.below must be a number");
            cond.below = c["below"].get<double>();
        }
        if (!cond.above && !cond.below) throw std::invalid_argument(w + ".metric needs above or below");
    } else if (wit.key() == "event") {
        cond.kind = Condition::Event;
        only_keys(c, {"worker", "code"}, w + ".event");
        if (!c.contains("code") || !c["code"].is_string()) throw std::invalid_argument(w + ".event needs \"code\"");
        cond.code = c["code"].get<std::string>();
        cond.leaf = ++leaves;
    } else if (wit.key() == "system") {
        cond.kind = Condition::System;
        only_keys(c, {"is"}, w + ".system");
        if (!c.contains("is")) throw std::invalid_argument(w + ".system needs \"is\"");
        cond.is = string_set(c["is"], w + ".system.is");
    } else {
        throw std::invalid_argument(w + ": unknown condition " + wit.key());
    }
    if (c.contains("worker")) {
        if (!c["worker"].is_string()) throw std::invalid_argument(w + " worker must be a glob string");
        cond.worker = c["worker"].get<std::string>();
    }
    return cond;
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
        only_keys(r, {"id", "when", "for_s", "hold_s", "severity", "text", "do", "stages"}, w);
        if (!r.contains("when")) throw std::invalid_argument(w + ".when must have exactly one of health, metric, event, system, all, any");
        int leaves = 0;
        rule.when = parse_condition(r["when"], w + ".when", 0, leaves);
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
        if (r.contains("do")) rule.actions = parse_actions(r["do"], w + ".do");
        if (r.contains("stages")) {
            if (!r["stages"].is_array() || r["stages"].empty()) throw std::invalid_argument(w + ".stages must be a non-empty list");
            double last_after = 0;
            int last_rank = severity_rank(rule.severity);
            for (size_t i = 0; i < r["stages"].size(); ++i) {
                const Json& j = r["stages"][i];
                const std::string sw = w + ".stages[" + std::to_string(i) + "]";
                if (!j.is_object()) throw std::invalid_argument(sw + " must be an object");
                only_keys(j, {"after_s", "severity", "text", "do"}, sw);
                Stage st;
                if (!j.contains("after_s") || !j["after_s"].is_number() || j["after_s"].get<double>() <= last_after)
                    throw std::invalid_argument(sw + ".after_s must be a number > 0 and larger than the stage before");
                st.after_s = last_after = j["after_s"].get<double>();
                if (!j.contains("severity") || !j["severity"].is_string() || severity_rank(j["severity"].get<std::string>()) == 0)
                    throw std::invalid_argument(sw + ".severity must be warning, error or critical");
                st.severity = j["severity"].get<std::string>();
                if (severity_rank(st.severity) < last_rank)
                    throw std::invalid_argument(sw + ".severity must not be lower than the stage before");
                last_rank = severity_rank(st.severity);
                if (j.contains("text")) {
                    if (!j["text"].is_string()) throw std::invalid_argument(sw + ".text must be a string");
                    st.text = j["text"].get<std::string>();
                }
                if (j.contains("do")) st.actions = parse_actions(j["do"], sw + ".do");
                rule.stages.push_back(st);
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

void Engine::note_event(const Rule& r, const Condition& c, const std::string& src, const std::string& code, int64_t now) {
    if (c.kind == Condition::All || c.kind == Condition::Any) {
        for (const auto& ch : c.children) note_event(r, ch, src, code, now);
        return;
    }
    if (c.kind == Condition::Event && glob(c.worker, src) && glob(c.code, code))
        leaf_event_at_[r.id + "#" + std::to_string(c.leaf)][src] = now;
}

void Engine::on_event(const std::string& src, const Json& event, int64_t now_ns) {
    if (!event.is_object()) return;
    const std::string code = event.value("code", "");
    for (const auto& r : rules_) {
        if (r.combined()) note_event(r, r.when, src, code, now_ns);
        else if (r.when.kind == Condition::Event && glob(r.when.worker, src) && glob(r.when.code, code))
            event_at_[r.id + ":" + src] = now_ns;
    }
}

// 조합 판정(시스템 전체: 조건마다 맞는 워커가 하나라도 있으면 참). status가 없으면 건강·시스템은 모름
Engine::Tri Engine::eval(const Rule& r, const Condition& c, int64_t now) const {
    switch (c.kind) {
    case Condition::All:
    case Condition::Any: {
        bool unknown = false;
        for (const auto& ch : c.children) {
            const Tri t = eval(r, ch, now);
            if (c.kind == Condition::All && t == Tri::False) return Tri::False;
            if (c.kind == Condition::Any && t == Tri::True) return Tri::True;
            unknown |= t == Tri::Unknown;
        }
        if (unknown) return Tri::Unknown;
        return c.kind == Condition::All ? Tri::True : Tri::False;
    }
    case Condition::Health:
        if (!status_link_) return Tri::Unknown;
        if (health_.is_object())
            for (auto it = health_.begin(); it != health_.end(); ++it)
                if (glob(c.worker, it.key()) && it->is_object() && c.is.count(it->value("health", ""))) return Tri::True;
        return Tri::False;
    case Condition::System:
        if (!status_link_) return Tri::Unknown;
        return c.is.count(system_health_) ? Tri::True : Tri::False;
    case Condition::Metric:
        for (const auto& [w, m] : metrics_) {
            if (!glob(c.worker, w) || !m.is_object() || !m.contains(c.name) || !m[c.name].is_number()) continue;
            const double v = m[c.name].get<double>();
            if ((c.above && v > *c.above) || (c.below && v < *c.below)) return Tri::True;
        }
        return Tri::False;
    case Condition::Event: {
        const auto it = leaf_event_at_.find(r.id + "#" + std::to_string(c.leaf));
        if (it != leaf_event_at_.end())
            for (const auto& [w, t] : it->second)
                if (now - t < static_cast<int64_t>(r.hold_s * 1e9)) return Tri::True;
        return Tri::False;
    }
    }
    return Tri::False;
}

void Engine::raise(const std::string& key, const Rule& r, const std::string& worker, int64_t now, bool operation, Out& out) {
    Active a;
    a.rule = r.id;
    a.worker = worker;
    a.severity = r.severity;
    a.text = r.text + (worker.empty() ? "" : " (" + worker + ")");
    a.since = now;
    out.events.push_back({{"severity", r.severity}, {"code", "fault_raised"}, {"text", a.text},
                          {"detail", {{"fault", key}, {"rule", r.id}, {"worker", worker.empty() ? Json(nullptr) : Json(worker)}}}});
    add_actions(a, key, r.actions, 0, operation, now, out);  // 잡아 둔 이벤트는 fault_raised 뒤에
    active_[key] = std::move(a);
}

void Engine::add_actions(Active& a, const std::string& key, const std::vector<Action>& acts, size_t stage, bool operation, int64_t now,
                         Out& out) {
    if (acts.empty()) return;
    Json held = Json::array();
    for (const Action& act : acts) {
        Json e = {{"target", act.target}, {"cmd", act.cmd}, {"ok", nullptr}};
        if (stage > 0) e["stage"] = stage;
        if (operation) {
            out.sends.push_back({key, a.actions.size(), act});
        } else {  // 개발 모드: 보내지 않고 잡아 둠(docs/modes.md 3장)
            e["held"] = true;
            a.held[stage].at = now;
            a.held[stage].actions.push_back({a.actions.size(), act});
            held.push_back({{"target", act.target}, {"cmd", act.cmd}, {"args", act.args}});
        }
        a.actions.push_back(e);
    }
    if (!operation)
        out.events.push_back({{"severity", "notice"}, {"code", "fault_action_held"},
                              {"text", a.text + ": action held (development, fault.run_action)"},
                              {"detail", {{"fault", key}, {"rule", a.rule}, {"stage", stage}, {"actions", held}}}});
}

bool Engine::run_held(const std::string& fault, size_t stage, std::vector<Send>* sends) {
    auto it = active_.find(fault);
    if (it == active_.end()) return false;
    auto h = it->second.held.find(stage);
    if (h == it->second.held.end()) return false;
    for (const auto& [index, act] : h->second.actions) {
        if (index < it->second.actions.size()) {
            it->second.actions[index].erase("held");
            it->second.actions[index]["manual"] = true;  // 사람이 fault.run_action으로 보냄
        }
        if (sends) sends->push_back({fault, index, act});
    }
    it->second.held.erase(h);
    return true;
}

bool Engine::has_held(const std::string& fault, size_t stage) const {
    auto it = active_.find(fault);
    return it != active_.end() && it->second.held.count(stage) > 0;
}

Json Engine::held(int64_t now_ns) const {
    Json out = Json::array();
    for (const auto& [key, a] : active_)
        for (const auto& [stage, h] : a.held) {
            Json acts = Json::array();
            for (const auto& [index, act] : h.actions) acts.push_back({{"target", act.target}, {"cmd", act.cmd}, {"args", act.args}});
            out.push_back({{"id", key}, {"stage", stage}, {"actions", acts}, {"held_ns", h.at}, {"age_ms", (now_ns - h.at) / 1000000}});
        }
    return out;
}

size_t Engine::held_count() const {
    size_t n = 0;
    for (const auto& [key, a] : active_) n += a.held.size();
    return n;
}

// 단계 대응: 올라간 때부터 after_s가 지난 단계마다 한 번(심각도·말을 바꾸고 그 단계 대응)
void Engine::escalate(int64_t now, bool operation, Out& out) {
    for (auto& [key, a] : active_) {
        const Rule* r = nullptr;
        for (const auto& x : rules_)
            if (x.id == a.rule) r = &x;
        if (!r) continue;
        while (a.stage < r->stages.size() && now - a.since >= static_cast<int64_t>(r->stages[a.stage].after_s * 1e9)) {
            const Stage& st = r->stages[a.stage];
            ++a.stage;
            a.severity = st.severity;
            if (!st.text.empty()) a.text = st.text + (a.worker.empty() ? "" : " (" + a.worker + ")");
            out.events.push_back({{"severity", st.severity}, {"code", "fault_escalated"}, {"text", a.text},
                                  {"detail", {{"fault", key}, {"rule", a.rule}, {"stage", a.stage}}}});
            add_actions(a, key, st.actions, a.stage, operation, now, out);
        }
    }
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
        if (r.combined()) {
            const Tri t = eval(r, r.when, now_ns);
            if (t == Tri::Unknown) {  // 판정할 수 없음: 올라간 이상은 그대로 둠
                if (active_.count(r.id)) seen.insert(r.id);
            } else {
                consider(r, "", t == Tri::True);
            }
            // 오래된 이벤트 기록 지우기
            for (auto it = leaf_event_at_.begin(); it != leaf_event_at_.end(); ++it) {
                if (it->first.rfind(r.id + "#", 0) != 0) continue;
                for (auto e = it->second.begin(); e != it->second.end();)
                    e = now_ns - e->second >= static_cast<int64_t>(r.hold_s * 1e9) ? it->second.erase(e) : std::next(e);
            }
            continue;
        }
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
        case Condition::All:
        case Condition::Any:
            break;  // 위에서
        }
    }
    // status 끊김(스스로 올리는 이상)
    static const Rule lost{"status_lost", "warning", "status-manager lost", {}, 0, 0, {}, {}};
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
    escalate(now_ns, operation, out);
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
                  {"stage", a.stage}, {"actions", a.actions}};
        counts[a.severity] = counts[a.severity].get<int>() + 1;
    }
    return {{"active", act}, {"counts", counts}, {"worst", worst()}, {"held_actions", held(now_ns)}};
}

}  // namespace fault
