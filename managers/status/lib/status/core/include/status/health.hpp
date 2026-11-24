// 건강 판정 엔진(managers/status/design.md 2장). 워커 상태(process-manager 구독의 worker_status) → 워커마다 건강
// → 시스템 건강 하나. 규칙(필수 워커, heartbeat 기준, 지표 조건)은 데이터(매니저 설정, 원칙 18).
//
//   워커:   off(꺼 둠) > fault(실패·연속 실패·heartbeat 끊김) > starting > degraded(*_link 지표 0, 지표 조건) > ok
//   시스템: fault(필수 워커가 fault·off) > degraded(필수 워커가 degraded·starting, 필수 아닌 워커가 fault,
//           process-manager 끊김) > ok
//   필수:   규칙 required(워커 id 글롭). 비면 autostart 워커 전부
//
// 시스템 호출·시계를 모른다(now_ns는 인자로)
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace status {

using Json = nlohmann::json;

struct MetricRule {
    std::string worker = "*";  // 워커 id 글롭
    std::string metric;        // heartbeat 지표 이름
    std::optional<double> below, above;
    std::string then = "degraded";  // degraded | fault
};

struct Rules {
    std::vector<std::string> required;  // 비면 autostart 워커
    double stale_s = 3.0;               // heartbeat가 이만큼 없으면 fault
    std::vector<MetricRule> metrics;
};

// 설정 칸 → 규칙(틀리면 std::invalid_argument)
Rules parse_rules(const Json& config);

// 건강 순서(큰 것이 나쁨)
int severity_of(const std::string& health);

class Health {
public:
    explicit Health(Rules rules = {}) : rules_(std::move(rules)) {}

    void on_worker(const Json& data, int64_t now_ns);  // worker_status의 data
    void on_removed(const std::string& id);
    void set_pm_link(bool up) { pm_link_ = up; }
    void set_stream(const std::string& id, bool up) { stream_[id] = up; }
    void forget_stream(const std::string& id) { stream_.erase(id); }

    struct Result {
        Json system;              // 주제 system(clock 빼고)
        std::vector<Json> events; // 바뀐 것(공통 이벤트 형식, seq·t_mono_ns 붙임)
    };
    // 지금 판정. 이전 판정과 다른 시스템·워커 건강은 이벤트로
    Result evaluate(int64_t now_ns);

    const Json& last() const { return last_; }

private:
    struct Worker {
        Json data;
        int64_t received_ns = 0;
    };
    std::string worker_health(const std::string& id, const Worker& w, int64_t now_ns, Json& links, std::string& why) const;
    bool required(const std::string& id, const Worker& w) const;

    Rules rules_;
    std::map<std::string, Worker> workers_;
    std::map<std::string, bool> stream_;
    bool pm_link_ = false;
    std::string last_system_;                      // 이전 시스템 건강(처음엔 빈 글)
    std::map<std::string, std::string> last_worker_;  // 이전 워커 건강
    uint64_t seq_ = 0;
    Json last_ = Json::object();
};

}  // namespace status
