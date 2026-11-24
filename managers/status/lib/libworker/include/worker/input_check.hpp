// 받은 칸 실행 중 검사(워크스페이스 docs/input-check.md, libworker 0.17).
//
// 호스트가 시작할 때 계약(io.json uses + io/uses/<역할>.json 사본)으로 표를 만든다: 상대 워커 id → 주제 → 스키마.
// Subscriber가 줄을 받을 때마다 observe()를 부르고, 주제마다 period마다 한 번 스키마로 검사한다(데이터는 그대로 넘김:
// 막지 않고 알리기만). 틀리면 세고 마지막 이유를 남기며, (입력, 주제)마다 60 s에 한 번 이벤트 input_mismatch.
// 표가 없으면(단독 실행·계약 없음·설정 input_check "off") 아무것도 안 함. 프로세스 하나에 표 하나(호스트 하나).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace worker {
namespace input_check {

struct Topic {
    std::string input;    // io.json uses의 입력 이름
    nlohmann::json schema;
    nlohmann::json defs;  // 사본의 definitions(+ 공통)
};
// 상대 워커 id → 주제 이름(자리 <…> 가능) → 스키마
using Table = std::map<std::string, std::map<std::string, Topic>>;

void set(Table table, double period_s);
void clear();
bool active();

// Subscriber 스레드가 부른다(줄의 topics 객체)
void observe(const std::string& peer, const nlohmann::json& topics);

struct Stat {
    uint64_t checked = 0, bad = 0;
    std::string last_error;  // "<주제>: <경로>: <이유>"
};
std::map<std::string, Stat> stats();  // 입력 → 셈(표에 있는 입력 모두)
// 알릴 것(입력·주제마다 60 s에 한 번): {"input", "topic", "peer", "errors": [...]}
std::vector<nlohmann::json> take_mismatches();

// 호스트가 부름: 계약 묶음(io_bundle)과 입력 주소(입력 → 상대 워커 id)로 표를 만든다
Table table_from_bundle(const nlohmann::json& bundle, const std::map<std::string, std::string>& input_peer);

}  // namespace input_check
}  // namespace worker
