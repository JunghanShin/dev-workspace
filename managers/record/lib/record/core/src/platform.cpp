#include "record/platform.hpp"

namespace record {

const std::vector<std::string>& PlatformRecorder::state_keys() {
    static const std::vector<std::string> keys = {"desired_state", "actual_state", "pid", "last_exit_code", "last_signal",
                                                  "last_error", "restart_count", "config_version", "applied_version"};
    return keys;
}

static Json get_or_null(const Json& o, const std::string& k) {
    if (!o.is_object()) return nullptr;
    auto it = o.find(k);
    return it == o.end() ? Json(nullptr) : *it;
}

std::vector<Json> PlatformRecorder::handle(const Json& msg, double now_s) {
    std::vector<Json> out;
    if (!msg.is_object()) return out;
    const Json event = get_or_null(msg, "event");
    const Json data = get_or_null(msg, "data");
    if (!event.is_string()) return out;
    const std::string ev = event.get<std::string>();

    if (ev == "worker_status") {
        if (!data.is_object()) return out;
        const Json wid_j = get_or_null(data, "id");
        const std::string wid = wid_j.is_string() ? wid_j.get<std::string>() : wid_j.dump();
        Json state = Json::array();
        for (const auto& k : state_keys()) state.push_back(get_or_null(data, k));
        auto ls = last_state_.find(wid);
        const bool changed = ls == last_state_.end() || ls->second != state;
        auto lw = last_written_.find(wid);
        const double last = lw == last_written_.end() ? -1e9 : lw->second;
        const Json st = get_or_null(data, "actual_state");
        const double every = st.is_string() && st.get<std::string>() == "running" ? interval_ : stopped_interval_;
        if (changed || now_s - last >= every) {
            // 예전 logger: {"type": "worker", "changed": .., **data}, data의 type은 빼서 맨 끝 worker_type으로
            Json rec = Json::object();
            rec["type"] = "worker";
            rec["changed"] = changed;
            Json worker_type = nullptr;
            for (auto it = data.begin(); it != data.end(); ++it) {
                if (it.key() == "type") {
                    worker_type = it.value();
                    continue;
                }
                rec[it.key()] = it.value();
            }
            rec["worker_type"] = worker_type;
            out.push_back(std::move(rec));
            last_state_[wid] = std::move(state);
            last_written_[wid] = now_s;
        }
    } else if (ev == "worker_removed") {
        const Json wid_j = get_or_null(data, "id");
        const std::string wid = wid_j.is_string() ? wid_j.get<std::string>() : wid_j.dump();
        last_state_.erase(wid);
        last_written_.erase(wid);
        Json rec = Json::object();
        rec["type"] = "removed";
        rec["id"] = wid_j;
        out.push_back(std::move(rec));
    } else if (ev == "command") {
        Json rec = Json::object();
        rec["type"] = "command";
        if (data.is_object())
            for (auto it = data.begin(); it != data.end(); ++it) rec[it.key()] = it.value();
        out.push_back(std::move(rec));
    } else if (ev == "system") {
        if (data != last_system_) {  // 예전 logger와 같음(처음은 null과 비교)
            last_system_ = data;
            Json rec = Json::object();
            rec["type"] = "system";
            if (data.is_object())
                for (auto it = data.begin(); it != data.end(); ++it) rec[it.key()] = it.value();
            out.push_back(std::move(rec));
        }
    }
    return out;
}

Json PlatformRecorder::connected() {
    connected_ = true;
    Json rec = Json::object();
    rec["type"] = "logger";
    rec["event"] = "connected";
    return rec;
}

std::vector<Json> PlatformRecorder::disconnected(const std::string& reason) {
    std::vector<Json> out;
    if (connected_) {
        Json rec = Json::object();
        rec["type"] = "logger";
        rec["event"] = "disconnected";
        rec["reason"] = reason;
        out.push_back(std::move(rec));
    }
    connected_ = false;
    last_state_.clear();
    last_written_.clear();
    last_system_ = nullptr;
    return out;
}

}  // namespace record
