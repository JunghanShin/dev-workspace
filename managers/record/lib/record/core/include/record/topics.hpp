// 주제·이벤트 기록 규칙(managers/record/design.md 2.1·2.2). 워커·매니저 스트림 한 줄 → 기록들.
//
//   topic  규칙(topics[].match, 위에서부터 처음 맞는 것)의 간격마다 (보낸 곳, 주제)에 하나. kf(기준 상태)는 늘.
//          exclude에 맞으면 남기지 않음. 값의 t_mono_ns(잰 시각)가 있으면 그것, 없으면 받은 시각
//   event  모두(솎지 않음). 이벤트의 t_mono_ns가 있으면 그것. 기준 상태(kf)마다 최근 이벤트가 다시 실려 오므로
//          (보낸 곳, 이벤트 source)마다 마지막으로 남긴 것보다 새것만 남긴다: seq가 더 크거나 t_mono_ns가 더 늦으면 새것
//          (한 번에 난 이벤트들은 시각이 같아 seq로 가리고, 낸 쪽이 다시 시작해 seq가 처음부터면 시각으로 가림).
//          seq가 없으면 시각만 본다. 끊겨도 잊지 않음(다시 붙으면 또 실려 옴)
//
// 시스템 호출·시계를 모른다(now_ns는 인자로)
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace record {

using Json = nlohmann::ordered_json;

struct Stamped {
    Json rec;             // 기록(시각 칸 없음)
    int64_t t_mono_ns;    // 줄의 t_mono_ns
};

class TopicRules {
public:
    TopicRules();
    // topics: [{"match": "<보낸 곳>.<주제> 글롭", "interval_ms": n}], exclude: ["글롭", ...]. 틀리면 std::invalid_argument
    void configure(const Json& topics, const Json& exclude);
    // 이 (보낸 곳, 주제)를 지금 남기나. kf면 늘(제외만 빼고)
    bool take(const std::string& src, const std::string& topic, int64_t now_ns, bool kf);
    // 보낸 곳이 끊기거나 빠짐: 그 곳의 마지막 시각을 잊는다(다시 붙으면 바로 남김)
    void forget(const std::string& src);

    // 스트림 한 줄(topics 객체, events 배열, kf) → 기록들
    std::vector<Stamped> records(const std::string& src, const Json& topics, const Json& events, bool kf, int64_t now_ns);

private:
    struct Rule {
        std::string match;
        int64_t interval_ns;
    };
    std::vector<Rule> rules_;
    std::vector<std::string> exclude_;
    std::map<std::pair<std::string, std::string>, int64_t> last_;  // (보낸 곳, 주제) → 마지막으로 남긴 시각
    struct LastEvent {
        int64_t t = 0;
        int64_t seq = -1;
    };
    std::map<std::pair<std::string, std::string>, LastEvent> last_event_;  // (보낸 곳, 이벤트 source) → 마지막으로 남긴 것
};

}  // namespace record
