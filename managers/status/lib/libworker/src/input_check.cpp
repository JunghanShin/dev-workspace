#include "worker/input_check.hpp"

#include <atomic>
#include <chrono>
#include <mutex>

#include "worker/schema.hpp"

namespace worker {
namespace input_check {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

struct State {
    std::mutex mu;
    Table table;
    std::chrono::nanoseconds period{std::chrono::seconds(1)};
    std::map<std::string, Clock::time_point> last_check;   // peer \x1f topic
    std::map<std::string, Clock::time_point> last_notice;  // input \x1f topic
    std::map<std::string, Stat> stats;
    std::vector<Json> pending;
};
State& st() {
    static State s;
    return s;
}
std::atomic<bool> g_active{false};
constexpr auto kNoticeEvery = std::chrono::seconds(60);

const Topic* find_topic(const std::map<std::string, Topic>& topics, const std::string& name) {
    if (auto it = topics.find(name); it != topics.end()) return &it->second;
    for (const auto& [pat, t] : topics)
        if (pat.find('<') != std::string::npos && name_matches(pat, name)) return &t;
    return nullptr;
}

}  // namespace

void set(Table table, double period_s) {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    s.table = std::move(table);
    s.period = std::chrono::nanoseconds(static_cast<int64_t>(period_s * 1e9));
    s.last_check.clear();
    s.last_notice.clear();
    s.stats.clear();
    s.pending.clear();
    for (const auto& [peer, topics] : s.table)
        for (const auto& [name, t] : topics) s.stats[t.input];
    g_active = !s.table.empty();
}

void clear() { set({}, 1.0); }

bool active() { return g_active.load(); }

void observe(const std::string& peer, const Json& topics) {
    if (!g_active.load() || !topics.is_object()) return;
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    const auto p = s.table.find(peer);
    if (p == s.table.end()) return;
    const auto now = Clock::now();
    for (const auto& [name, value] : topics.items()) {
        if (value.is_null()) continue;  // 지움 표시
        const Topic* t = find_topic(p->second, name);
        if (!t) continue;  // 쓰지 않는 주제
        const std::string key = peer + "\x1f" + name;
        auto& last = s.last_check[key];
        if (last != Clock::time_point{} && now - last < s.period) continue;
        last = now;
        std::vector<std::string> errs;
        validate_schema(value, t->schema, t->defs, name, errs);
        Stat& stat = s.stats[t->input];
        ++stat.checked;
        if (errs.empty()) continue;
        ++stat.bad;
        stat.last_error = errs.front();
        const std::string nkey = t->input + "\x1f" + name;
        auto& ln = s.last_notice[nkey];
        if (ln != Clock::time_point{} && now - ln < kNoticeEvery) continue;
        ln = now;
        if (s.pending.size() < 32) s.pending.push_back({{"input", t->input}, {"topic", name}, {"peer", peer}, {"errors", errs}});
    }
}

std::map<std::string, Stat> stats() {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    return s.stats;
}

std::vector<Json> take_mismatches() {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    std::vector<Json> out;
    out.swap(s.pending);
    return out;
}

Table table_from_bundle(const Json& bundle, const std::map<std::string, std::string>& input_peer) {
    Table out;
    if (!bundle.is_object() || !bundle.contains("uses") || !bundle["uses"].is_object()) return out;
    Json common_defs = Json::object();
    if (bundle.contains("common") && bundle["common"].is_object() && bundle["common"].contains("definitions"))
        common_defs = bundle["common"]["definitions"];
    for (const auto& [input, u] : bundle["uses"].items()) {
        if (!u.is_object() || u.value("many", false)) continue;  // 여럿에서 받는 입력: 역할이 섞여 건너뜀
        const auto peer = input_peer.find(input);
        if (peer == input_peer.end() || peer->second.empty()) continue;
        const Json def = u.value("definition", Json());
        if (!def.is_object() || !def.contains("topics") || !def["topics"].is_object()) continue;
        Json defs = common_defs;
        if (def.contains("definitions") && def["definitions"].is_object())
            for (const auto& [k, v] : def["definitions"].items()) defs[k] = v;
        for (const auto& t : u.value("topics", Json::array())) {
            if (!t.is_string()) continue;
            const std::string name = t.get<std::string>();
            if (!def["topics"].contains(name)) continue;
            const Json& td = def["topics"][name];
            if (!td.is_object() || !td.contains("schema")) continue;  // 스키마 없는 주제는 검사 안 함(출력 대조와 같음)
            out[peer->second][name] = Topic{input, td["schema"], defs};
        }
    }
    return out;
}

}  // namespace input_check
}  // namespace worker
