// 이상 판단 엔진(managers/fault/design.md 2·3장). 시스템 호출·시계를 모른다(시각은 인자로).
//
//   규칙: 조건 하나(health | metric | event | system) → 심각도 → 대응(do: mission.send로 보낼 명령들)
//   올림: 조건이 for_s 넘게 이어짐(event는 받는 즉시, hold_s 지나면 내림). 올릴 때 대응 한 번(운용 모드에서만)
//   워커 글롭에 여럿이 맞으면 워커마다 따로(이상 이름 "<규칙>:<워커>")
//   status가 3 s 넘게 끊기면 이상 status_lost(warning)를 스스로 올림
//   조합·단계(managers/fault/escalate.md): when {"all"|"any": [조건...]}(중첩, 시스템 전체로 한 번 판정, 이름 = 규칙 id),
//   stages [{after_s, severity, text?, do?}]: 올라간 뒤 after_s가 지나면 심각도를 올리고 그 단계 대응을 한 번
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace fault {

using Json = nlohmann::json;

struct Condition {
    enum Kind { Health, Metric, Event, System, All, Any } kind = Health;
    std::vector<Condition> children;  // All·Any
    int leaf = 0;                     // 조합 안 이벤트 조건 번호(규칙 안에서 1부터)
    std::string worker = "*";       // 워커 id 글롭(health·metric·event)
    std::set<std::string> is;       // health·system: 이 건강들 중 하나면 참
    std::string name;               // metric 이름
    std::optional<double> above, below;
    std::string code;               // event code(글롭)
};

struct Action {
    std::string target, cmd;
    Json args = Json::object();
};

struct Stage {
    double after_s = 0;  // 올라간 때부터
    std::string severity, text;  // text가 비면 그대로
    std::vector<Action> actions;
};

struct Rule {
    std::string id, severity = "warning", text;
    Condition when;
    double for_s = 0, hold_s = 10;
    std::vector<Action> actions;
    std::vector<Stage> stages;  // after_s 오름차순, 심각도는 내려가지 않음
    bool combined() const { return when.kind == Condition::All || when.kind == Condition::Any; }
};

// 설정 config["rules"] → 규칙(틀리면 std::invalid_argument)
std::vector<Rule> parse_rules(const Json& rules);
int severity_rank(const std::string& s);  // none 0, warning 1, error 2, critical 3

class Engine {
public:
    explicit Engine(std::vector<Rule> rules = {}) : rules_(std::move(rules)) {}

    // status 주제 system(null이면 끊김)
    void set_system(const Json& system, int64_t now_ns);
    void set_metrics(const std::string& worker, const Json& metrics);
    void remove_worker(const std::string& worker);
    void on_event(const std::string& src, const Json& event, int64_t now_ns);

    struct Send {
        std::string fault;  // 이상 이름
        size_t index;       // actions 칸 번호
        Action action;
    };
    struct Out {
        std::vector<Json> events;  // fault_raised·fault_cleared(공통 이벤트 형식의 code·severity·text·detail)
        std::vector<Send> sends;   // 지금 보낼 대응
    };
    // 판정. operation: 운용 모드면 대응을 보냄, 아니면 skipped로 남김
    Out tick(int64_t now_ns, bool operation);
    void action_result(const std::string& fault, size_t index, bool ok, const Json& error);
    bool ack(const std::string& fault);

    Json snapshot(int64_t now_ns) const;
    size_t active() const { return active_.size(); }
    std::string worst() const;

private:
    struct Active {
        std::string rule, worker, severity, text;
        int64_t since = 0;
        bool acked = false;
        size_t stage = 0;  // 0 = 처음, n = stages[n-1]까지 감
        Json actions = Json::array();
    };
    enum class Tri { False, True, Unknown };
    void raise(const std::string& key, const Rule& r, const std::string& worker, int64_t now, bool operation, Out& out);
    void clear(const std::string& key, Out& out);
    void add_actions(Active& a, const std::string& key, const std::vector<Action>& acts, size_t stage, bool operation, Out& out);
    void escalate(int64_t now, bool operation, Out& out);
    Tri eval(const Rule& r, const Condition& c, int64_t now) const;
    void note_event(const Rule& r, const Condition& c, const std::string& src, const std::string& code, int64_t now);

    std::vector<Rule> rules_;
    Json health_ = Json();           // system.workers
    std::string system_health_;
    bool status_link_ = false;
    int64_t status_lost_since_ = 0;  // 0이면 붙어 있음
    std::map<std::string, Json> metrics_;
    std::map<std::string, int64_t> event_at_;   // "<규칙>:<워커>" → 마지막 이벤트 시각
    std::map<std::string, std::map<std::string, int64_t>> leaf_event_at_;  // 조합: "<규칙>#<번호>" → 워커 → 마지막 이벤트
    std::map<std::string, int64_t> true_since_; // 조건이 참이 된 시각
    std::map<std::string, Active> active_;
};

}  // namespace fault
