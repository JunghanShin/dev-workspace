#include "record/topics.hpp"

#include <fnmatch.h>

#include <stdexcept>

namespace record {

namespace {

bool glob(const std::string& pattern, const std::string& text) { return fnmatch(pattern.c_str(), text.c_str(), 0) == 0; }

int64_t stamp_of(const Json& v, int64_t fallback) {
    if (v.is_object()) {
        auto it = v.find("t_mono_ns");
        if (it != v.end() && it->is_number_integer()) return it->get<int64_t>();
    }
    return fallback;
}

}  // namespace

// 규칙이 없을 때(retention.md): 워커 상태를 다시 요약하는 관리 주제는 5 s, 그 밖 1 s
static std::vector<TopicRules::Rule> default_rules() {
    return {{"status-manager.*", 5000LL * 1000000}, {"payload-manager.*", 5000LL * 1000000}, {"*", 1000LL * 1000000}};
}

TopicRules::TopicRules() { rules_ = default_rules(); }

void TopicRules::configure(const Json& topics, const Json& exclude) {
    std::vector<Rule> rules;
    if (!topics.is_null()) {
        if (!topics.is_array()) throw std::invalid_argument("record.topics must be a list [{\"match\", \"interval_ms\"}]");
        for (const auto& t : topics) {
            if (!t.is_object() || !t.contains("match") || !t["match"].is_string())
                throw std::invalid_argument("record.topics items need \"match\" (glob <source>.<topic>)");
            for (auto it = t.begin(); it != t.end(); ++it)
                if (it.key() != "match" && it.key() != "interval_ms")
                    throw std::invalid_argument("record.topics: unknown key " + it.key());
            double ms = 1000;
            if (t.contains("interval_ms")) {
                if (!t["interval_ms"].is_number() || t["interval_ms"].get<double>() < 0)
                    throw std::invalid_argument("record.topics interval_ms must be a number >= 0");
                ms = t["interval_ms"].get<double>();
            }
            rules.push_back({t["match"].get<std::string>(), static_cast<int64_t>(ms * 1e6)});
        }
    } else {
        rules = default_rules();
    }
    std::vector<std::string> ex;
    if (!exclude.is_null()) {
        if (!exclude.is_array()) throw std::invalid_argument("record.exclude must be a list of globs");
        for (const auto& e : exclude) {
            if (!e.is_string()) throw std::invalid_argument("record.exclude items must be strings");
            ex.push_back(e.get<std::string>());
        }
    }
    rules_ = std::move(rules);
    exclude_ = std::move(ex);
    last_.clear();
}

bool TopicRules::take(const std::string& src, const std::string& topic, int64_t now_ns, bool kf) {
    const std::string key = src + "." + topic;
    for (const auto& e : exclude_)
        if (glob(e, key)) return false;
    const Rule* rule = nullptr;
    for (const auto& r : rules_)
        if (glob(r.match, key)) {
            rule = &r;
            break;
        }
    if (!rule) return false;  // 어느 규칙에도 맞지 않으면 남기지 않음(규칙을 좁게 쓴 경우)
    auto& last = last_[{src, topic}];
    if (!kf && last != 0 && now_ns - last < rule->interval_ns) return false;
    last = now_ns;
    return true;
}

void TopicRules::forget(const std::string& src) {
    for (auto it = last_.begin(); it != last_.end();) it = it->first.first == src ? last_.erase(it) : std::next(it);
}

std::vector<Stamped> TopicRules::records(const std::string& src, const Json& topics, const Json& events, bool kf,
                                         int64_t now_ns) {
    std::vector<Stamped> out;
    if (topics.is_object())
        for (auto it = topics.begin(); it != topics.end(); ++it) {
            if (!take(src, it.key(), now_ns, kf)) continue;
            Json rec = Json::object();
            rec["type"] = "topic";
            rec["src"] = src;
            rec["topic"] = it.key();
            if (kf) rec["kf"] = true;
            rec["v"] = it.value();
            out.push_back({std::move(rec), stamp_of(it.value(), now_ns)});
        }
    if (events.is_array())
        for (const auto& e : events) {
            if (e.is_object() && e.contains("t_mono_ns") && e["t_mono_ns"].is_number_integer()) {
                const std::string source = e.contains("source") && e["source"].is_string() ? e["source"].get<std::string>() : "";
                const int64_t t = e["t_mono_ns"].get<int64_t>();
                const int64_t seq = e.contains("seq") && e["seq"].is_number_integer() ? e["seq"].get<int64_t>() : -1;
                auto& last = last_event_[{src, source}];
                const bool newer = t > last.t || (seq >= 0 && t == last.t && seq > last.seq);
                if (!newer) continue;  // 이미 남긴 것(kf에 다시 실려 옴)
                last.t = t;
                last.seq = seq;
            }
            Json rec = Json::object();
            rec["type"] = "event";
            rec["src"] = src;
            rec["v"] = e;
            out.push_back({std::move(rec), stamp_of(e, now_ns)});
        }
    return out;
}

}  // namespace record
