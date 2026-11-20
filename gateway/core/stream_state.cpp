#include "stream_state.hpp"

namespace gw {

namespace {

const Json& field_object(const Json& msg, const char* key) {
    static const Json empty = Json::object();
    auto it = msg.find(key);
    return (it != msg.end() && it->is_object()) ? *it : empty;
}

const Json& field_array(const Json& msg, const char* key) {
    static const Json empty = Json::array();
    auto it = msg.find(key);
    return (it != msg.end() && it->is_array()) ? *it : empty;
}

void trim_events(Json& events) {
    if (events.size() > kMaxEvents) events.erase(events.begin(), events.end() - static_cast<long>(kMaxEvents));
}

// 이벤트 번호(없거나 숫자가 아니면 0, Python e.get("seq", 0))
double event_seq(const Json& e) {
    if (!e.is_object()) return 0;
    auto it = e.find("seq");
    return (it != e.end() && it->is_number()) ? it->get<double>() : 0;
}

}  // namespace

void SseMerge::offer(const Json& msg, bool keyframe) {
    const Json& topics = field_object(msg, "topics");
    const Json& events = field_array(msg, "events");
    if (keyframe) {
        // 기준 상태는 쌓인 것을 대신한다
        kf_ = true;
        topics_ = topics;
        events_ = events;
    } else {
        for (auto it = topics.begin(); it != topics.end(); ++it) topics_[it.key()] = *it;
        for (const auto& e : events) events_.push_back(e);
        trim_events(events_);
    }
    for (const char* k : {"t", "events_next", "stream"}) {
        auto it = msg.find(k);
        if (it != msg.end()) extra_[k] = *it;
    }
    dirty_ = true;
}

Json SseMerge::take() {
    Json out = Json::object();
    out["seq"] = ++seq_;
    out["kf"] = kf_;
    out["topics"] = std::move(topics_);
    if (!events_.empty()) out["events"] = std::move(events_);
    for (auto it = extra_.begin(); it != extra_.end(); ++it) out[it.key()] = *it;
    kf_ = false;
    topics_ = Json::object();
    events_ = Json::array();
    extra_ = Json::object();
    dirty_ = false;
    return out;
}

Json UpstreamState::apply(const Json& msg, bool fresh) {
    const Json& events = field_array(msg, "events");
    if (fresh) {
        // 다시 붙은 뒤 첫 기준 상태: 워커가 새로 시작했을 수 있다(이벤트 번호도 처음부터)
        topics_ = Json::object();
        events_ = Json::array();
        last_event_ = 0;
    }
    const Json& topics = field_object(msg, "topics");
    for (auto it = topics.begin(); it != topics.end(); ++it) topics_[it.key()] = *it;
    Json fresh_events = Json::array();
    for (const auto& e : events)
        if (event_seq(e) > last_event_) fresh_events.push_back(e);
    if (!fresh_events.empty()) {
        last_event_ = event_seq(fresh_events.back());
        for (const auto& e : fresh_events) events_.push_back(e);
    }
    trim_events(events_);
    for (const char* k : {"t", "events_next"}) {
        auto it = msg.find(k);
        if (it != msg.end()) extra_[k] = *it;
    }
    Json out = Json::object();
    if (fresh) {
        out["topics"] = topics_;
        out["events"] = events_;
        out["stream"] = "up";
        for (auto it = extra_.begin(); it != extra_.end(); ++it) out[it.key()] = *it;
    } else {
        out["topics"] = topics;
        out["events"] = std::move(fresh_events);
        for (const char* k : {"t", "events_next"}) {
            auto it = msg.find(k);
            if (it != msg.end()) out[k] = *it;
        }
    }
    return out;
}

Json UpstreamState::snapshot(bool up) const {
    Json out = Json::object();
    out["topics"] = topics_;
    out["events"] = events_;
    out["stream"] = up ? "up" : "down";
    for (auto it = extra_.begin(); it != extra_.end(); ++it) out[it.key()] = *it;
    return out;
}

}  // namespace gw
