#include "status/health.hpp"

#include <fnmatch.h>

#include <stdexcept>

namespace status {

namespace {

bool glob(const std::string& p, const std::string& s) { return fnmatch(p.c_str(), s.c_str(), 0) == 0; }

std::string str(const Json& o, const char* k) {
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool truthy(const Json& o, const char* k) {
    auto it = o.find(k);
    return it != o.end() && it->is_boolean() && it->get<bool>();
}

const char* event_severity(const std::string& health) {
    if (health == "fault") return "error";
    if (health == "degraded" || health == "off") return "warning";
    return "info";
}

}  // namespace

int severity_of(const std::string& h) {
    if (h == "ok") return 0;
    if (h == "starting") return 1;
    if (h == "degraded") return 2;
    if (h == "off") return 3;
    if (h == "fault") return 4;
    return 0;
}

Rules parse_rules(const Json& c) {
    Rules r;
    if (c.is_null()) return r;
    if (!c.is_object()) throw std::invalid_argument("status config must be an object");
    for (auto it = c.begin(); it != c.end(); ++it)
        if (it.key() != "required" && it.key() != "stale_s" && it.key() != "metrics" && it.key() != "publish_ms")
            throw std::invalid_argument("unknown config key: status." + it.key());
    if (c.contains("required")) {
        if (!c["required"].is_array()) throw std::invalid_argument("status.required must be a list of worker id globs");
        for (const auto& x : c["required"]) {
            if (!x.is_string()) throw std::invalid_argument("status.required items must be strings");
            r.required.push_back(x.get<std::string>());
        }
    }
    if (c.contains("stale_s")) {
        if (!c["stale_s"].is_number() || c["stale_s"].get<double>() <= 0) throw std::invalid_argument("status.stale_s must be > 0");
        r.stale_s = c["stale_s"].get<double>();
    }
    if (c.contains("metrics")) {
        if (!c["metrics"].is_array()) throw std::invalid_argument("status.metrics must be a list");
        for (const auto& m : c["metrics"]) {
            if (!m.is_object() || !m.contains("metric") || !m["metric"].is_string())
                throw std::invalid_argument("status.metrics items need \"metric\"");
            MetricRule mr;
            mr.metric = m["metric"].get<std::string>();
            if (m.contains("worker")) {
                if (!m["worker"].is_string()) throw std::invalid_argument("status.metrics worker must be a glob string");
                mr.worker = m["worker"].get<std::string>();
            }
            if (m.contains("below")) {
                if (!m["below"].is_number()) throw std::invalid_argument("status.metrics below must be a number");
                mr.below = m["below"].get<double>();
            }
            if (m.contains("above")) {
                if (!m["above"].is_number()) throw std::invalid_argument("status.metrics above must be a number");
                mr.above = m["above"].get<double>();
            }
            if (!mr.below && !mr.above) throw std::invalid_argument("status.metrics needs below or above");
            if (m.contains("then")) {
                const Json t = m["then"];
                if (t != "degraded" && t != "fault") throw std::invalid_argument("status.metrics then must be degraded or fault");
                mr.then = t.get<std::string>();
            }
            for (auto it = m.begin(); it != m.end(); ++it)
                if (it.key() != "worker" && it.key() != "metric" && it.key() != "below" && it.key() != "above" && it.key() != "then")
                    throw std::invalid_argument("status.metrics: unknown key " + it.key());
            r.metrics.push_back(mr);
        }
    }
    return r;
}

void Health::on_worker(const Json& data, int64_t now_ns) {
    if (!data.is_object()) return;
    const std::string id = str(data, "id");
    if (id.empty()) return;
    workers_[id] = Worker{data, now_ns};
}

void Health::on_removed(const std::string& id) {
    workers_.erase(id);
    last_worker_.erase(id);
}

bool Health::required(const std::string& id, const Worker& w) const {
    if (rules_.required.empty()) return truthy(w.data, "autostart");
    for (const auto& g : rules_.required)
        if (glob(g, id)) return true;
    return false;
}

std::string Health::worker_health(const std::string& id, const Worker& w, int64_t now_ns, Json& links, std::string& why) const {
    const Json& d = w.data;
    const std::string actual = str(d, "actual_state"), desired = str(d, "desired_state");
    const Json metrics = d.contains("metrics") && d["metrics"].is_object() ? d["metrics"] : Json::object();
    for (auto it = metrics.begin(); it != metrics.end(); ++it) {
        const std::string& k = it.key();
        if (k.size() > 5 && k.compare(k.size() - 5, 5, "_link") == 0 && it->is_number()) links[k] = *it;
    }
    if (actual == "disabled" || (desired == "stopped" && (actual == "stopped" || actual.empty()))) {
        why = "off";
        return "off";
    }
    if (truthy(d, "crash_loop")) {
        why = "crash loop";
        return "fault";
    }
    if (actual == "error" || actual == "failed" || actual == "exited") {
        why = actual + (str(d, "last_error").empty() ? "" : " (" + str(d, "last_error") + ")");
        return "fault";
    }
    if (actual == "starting" || actual == "stopping") {
        why = actual;
        return "starting";
    }
    if (actual == "running") {
        double age_ms = -1;
        if (d.contains("last_heartbeat_ms_ago") && d["last_heartbeat_ms_ago"].is_number())
            age_ms = d["last_heartbeat_ms_ago"].get<double>() + (now_ns - w.received_ns) / 1e6;
        if (age_ms > rules_.stale_s * 1000) {
            why = "no heartbeat for " + std::to_string(static_cast<long long>(age_ms)) + " ms";
            return "fault";
        }
        std::string worst = "ok";
        for (const auto& r : rules_.metrics) {
            if (!glob(r.worker, id) || !metrics.contains(r.metric) || !metrics[r.metric].is_number()) continue;
            const double v = metrics[r.metric].get<double>();
            if ((r.below && v < *r.below) || (r.above && v > *r.above)) {
                if (severity_of(r.then) > severity_of(worst)) {
                    worst = r.then;
                    why = r.metric + "=" + metrics[r.metric].dump();
                }
            }
        }
        if (worst == "fault") return worst;
        for (auto it = links.begin(); it != links.end(); ++it)
            if (it->is_number() && it->get<double>() == 0) {
                why = "link down (" + it.key() + "=0)";
                return "degraded";
            }
        return worst;
    }
    why = actual.empty() ? "unknown" : actual;
    return "starting";
}

Health::Result Health::evaluate(int64_t now_ns) {
    Result res;
    Json workers = Json::object();
    Json counts = {{"ok", 0}, {"degraded", 0}, {"fault", 0}, {"starting", 0}, {"off", 0}};
    Json reasons = Json::array();
    std::string sys = "ok";
    auto raise = [&](const std::string& h) {
        if (severity_of(h) > severity_of(sys)) sys = h == "off" ? "fault" : h;
    };
    for (const auto& [id, w] : workers_) {
        Json links = Json::object();
        std::string why;
        const std::string h = worker_health(id, w, now_ns, links, why);
        const bool req = required(id, w);
        counts[h] = counts[h].get<int>() + 1;
        Json e = Json::object();
        e["health"] = h;
        e["state"] = str(w.data, "actual_state");
        e["kind"] = str(w.data, "kind").empty() ? "worker" : str(w.data, "kind");
        e["required"] = req;
        e["links"] = links;
        e["why"] = h == "ok" ? Json(nullptr) : Json(why);
        if (w.data.contains("last_heartbeat_ms_ago")) e["heartbeat_age_ms"] = w.data["last_heartbeat_ms_ago"];
        auto st = stream_.find(id);
        e["stream"] = st == stream_.end() ? Json(nullptr) : Json(st->second);
        workers[id] = e;
        // 시스템으로: 필수 워커 fault·off → fault, degraded·starting → degraded. 필수 아닌 워커 fault → degraded
        if (req && (h == "fault" || h == "off")) {
            raise("fault");
            reasons.push_back(id + ": " + h + " (" + why + ")");
        } else if (req && (h == "degraded" || h == "starting")) {
            raise("degraded");
            reasons.push_back(id + ": " + h + " (" + why + ")");
        } else if (!req && h == "fault") {
            raise("degraded");
            reasons.push_back(id + ": " + h + " (" + why + ", not required)");
        }
        // 워커 건강이 바뀜 → 이벤트
        auto lw = last_worker_.find(id);
        if (lw == last_worker_.end() || lw->second != h) {
            if (lw != last_worker_.end()) {  // 처음 본 것은 바뀜이 아님
                res.events.push_back({{"seq", ++seq_}, {"t_mono_ns", now_ns}, {"source", "status-manager.status"},
                                      {"severity", event_severity(h)}, {"code", "worker_health_changed"},
                                      {"text", id + ": " + lw->second + " -> " + h + (why.empty() || h == "ok" ? "" : " (" + why + ")")},
                                      {"detail", {{"worker", id}, {"from", lw->second}, {"to", h}}}});
            }
            last_worker_[id] = h;
        }
    }
    if (!pm_link_) {
        raise("degraded");
        reasons.push_back("process-manager link lost");
    }
    if (sys != last_system_) {
        res.events.push_back({{"seq", ++seq_}, {"t_mono_ns", now_ns}, {"source", "status-manager.status"},
                              {"severity", event_severity(sys)}, {"code", "health_changed"},
                              {"text", "system health " + (last_system_.empty() ? std::string("(start)") : last_system_) + " -> " + sys},
                              {"detail", {{"from", last_system_.empty() ? Json(nullptr) : Json(last_system_)}, {"to", sys}, {"reasons", reasons}}}});
        last_system_ = sys;
    }
    Json out = Json::object();
    out["t_mono_ns"] = now_ns;
    out["health"] = sys;
    out["reasons"] = reasons;
    out["workers"] = workers;
    out["counts"] = counts;
    out["pm_link"] = pm_link_;
    last_ = out;
    res.system = std::move(out);
    return res;
}

}  // namespace status
