// 명령 정리 엔진(managers/mission/design.md 2·3장). 시스템 호출·시계를 모른다(시각은 인자로).
//
//   역할 찾기: 대상 워커의 계약(get_io)으로 명령 → 역할. 역할 정의(provides)에 있으면 그 역할, 없으면 "self".
//             계약을 아직 모르면 명령 이름 앞부분("<모듈>.<명령>"의 모듈)으로 잠시 대신한다
//   대기 큐:  (대상, 역할)마다 한 번에 하나. 그동안 온 것은 큐(상한), 차면 busy. 큐에서 오래 기다리면 timeout
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mission {

using Json = nlohmann::json;

struct Contract {
    std::map<std::string, std::string> cmd_role;  // 명령 → 역할(provides)
    std::set<std::string> read;                    // 읽기 명령
};

// get_io 답(data) → 계약
Contract contract_from_io(const Json& io);
// 명령의 역할(계약이 없으면 nullptr → 이름 앞부분)
std::string role_of(const Contract* c, const std::string& cmd);

struct Request {
    uint64_t id = 0;
    std::string target, role, cmd, origin;
    Json args = Json::object();
    int64_t enq_ns = 0;
    int priority = 0;  // 클수록 먼저(priority.md). 규칙이 없으면 모두 같음
    std::string key() const { return target + "." + role; }
};

// 입구를 거쳐 명령하는 곳의 허락(managers/mission/external-link.md 4장). peers.json workers.<대상>.via에서.
//   via에 이름이 있는 곳만 검사한다(게이트웨이·매니저는 via에 없음: 이미 검사됐거나 플랫폼 규칙).
//   via에 없는 곳은 입구 allow에도 없어 워커 소켓 검사(enforce)가 먼저 막는다
struct Access {
    std::map<std::string, std::map<std::string, std::optional<std::set<std::string>>>> via;  // 대상 → 보낸 곳 → 명령(nullopt = 전부)
    std::set<std::string> listed;                                                           // via 어디엔가 있는 보낸 곳
};
Access access_from_peers(const Json& doc);
enum class Verdict { Pass, Allow, Deny };
// lease.renew {"role"}은 그 역할의 명령을 하나라도 보내도 되면 허락
Verdict check_access(const Access& a, const std::string& origin, const std::string& target, const std::string& cmd,
                     const Json& args, const Contract* c);
// 큐를 기다리지 않는 명령: 읽기(get_status, 계약 read), lease.renew
bool fast_command(const Contract* c, const std::string& cmd);

// 우선순위 규칙(managers/mission/priority.md): 보낸 곳 → 우선순위, 역할마다 잡아 두기(hold)
struct PriorityRules {
    int dflt = 50;
    std::map<std::string, int> origins;  // "screen", "fault-manager", "remote:*"(앞부분 일치) …
    int64_t hold_ns = 0;                 // 0이면 잡아 두기 없음
    std::map<std::string, int64_t> role_hold_ns;
    int priority_of(const std::string& origin) const;
    int64_t hold_of(const std::string& role) const;
};
// config["mission"]["priority"] → 규칙. 형식이 틀리면 std::runtime_error
PriorityRules priority_from_config(const Json& j);

// 명령 조건(managers/mission/conditions.md): 맞는 명령이 오면 require가 모두 참이어야 보냄
struct Condition {
    struct Need {
        std::string worker;  // 비우면 명령 대상
        std::string kind;    // "metric" | "state" | "mode"
        std::string key;     // 지표 이름(metric), 상태 값(state), 모드 값(mode)
        std::string op = "==";
        Json value;
    };
    std::string name, target = "*", cmd = "*", message;
    std::vector<Need> require;
    std::set<std::string> except;  // 검사하지 않는 보낸 곳(예 fault-manager 비상 대응)
    bool unknown_allow = false;    // 값을 모를 때(워커 없음·지표 없음) 기본은 막음
};
std::vector<Condition> conditions_from_config(const Json& j);
struct World {
    std::map<std::string, std::string> state;  // 워커 → actual_state
    std::map<std::string, Json> metrics;       // 워커 → 지표
    std::string mode;                          // 워크스페이스 운용/개발 모드(모르면 빈 글)
};
// 막히면 이유(이름·무엇이 어떤 값인지), 아니면 nullopt. 읽기 명령은 부르지 않음(빠른 줄)
std::optional<std::string> check_conditions(const std::vector<Condition>& cs, const World& w, const Request& r);

class Dispatcher {
public:
    Dispatcher(size_t queue_max = 4, int64_t total_timeout_ns = 2800LL * 1000000) : max_(queue_max), total_(total_timeout_ns) {}
    void set_rules(const PriorityRules& p) { rules_ = p; }

    enum class Admit { Send, Queued, Busy, Held };
    // 받기. Send면 지금 보낼 것(r 그대로), Queued면 앞이 끝나면, Busy면 거절(큐 참), Held면 거절(더 높은 곳이 잡아 둠).
    // 큐가 찼을 때 더 높은 우선순위가 오면 가장 낮은 대기 명령을 밀어냄(refused()로 받음)
    Admit submit(const Request& r);
    // (대상, 역할)의 보낸 명령이 끝남(응답·오류). 다음에 보낼 것이 있으면 돌려줌. now_ns < 0이면 마지막으로 안 시각
    std::optional<Request> done(const std::string& key, int64_t now_ns = -1);
    // 밀려나거나(preempted) 잡아 둔 동안 큐에서 빠진(held) 명령: (요청, 이유). 가져가면 비움
    std::vector<std::pair<Request, std::string>> refused();
    // 큐에서 total_timeout을 넘긴 것(보내지 않고 timeout으로 답할 것)
    std::vector<Request> expire(int64_t now_ns);
    // 쓰지 않는 키(보낸 것도 기다리는 것도 없음)
    bool idle(const std::string& key) const;

    Json snapshot(int64_t now_ns) const;
    size_t waiting() const;
    size_t keys() const { return lanes_.size(); }

private:
    struct Lane {
        std::optional<Request> inflight;
        int64_t inflight_since = 0;
        std::deque<Request> queue;  // 우선순위 높은 것부터, 같으면 온 순서
        std::string holder;         // 잡아 둔 곳
        int holder_prio = 0;
        int64_t held_until = 0;
    };
    bool held_against(const Lane& l, const Request& r, int64_t now) const {
        return l.held_until > now && r.priority < l.holder_prio && r.origin != l.holder;
    }
    PriorityRules rules_;
    std::vector<std::pair<Request, std::string>> refused_;
    size_t max_;
    int64_t total_;
    std::map<std::string, Lane> lanes_;
    int64_t last_now_ = 0;
};

}  // namespace mission
