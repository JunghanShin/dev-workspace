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
    std::string key() const { return target + "." + role; }
};

class Dispatcher {
public:
    Dispatcher(size_t queue_max = 4, int64_t total_timeout_ns = 2800LL * 1000000) : max_(queue_max), total_(total_timeout_ns) {}

    enum class Admit { Send, Queued, Busy };
    // 받기. Send면 지금 보낼 것(r 그대로), Queued면 앞이 끝나면, Busy면 거절
    Admit submit(const Request& r);
    // (대상, 역할)의 보낸 명령이 끝남(응답·오류). 다음에 보낼 것이 있으면 돌려줌
    std::optional<Request> done(const std::string& key);
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
        std::deque<Request> queue;
    };
    size_t max_;
    int64_t total_;
    std::map<std::string, Lane> lanes_;
    int64_t last_now_ = 0;
};

}  // namespace mission
