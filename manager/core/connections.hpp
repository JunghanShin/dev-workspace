// 입력 연결 풀기 (design.md 6.6.7.4). 시스템 호출 없이 표만 만든다(제어 루프에서 부름, test_core).
//
//   워커 계약(get_io: uses·provides) + 연결 선언(connections.json "connections") + 프리셋 inputs
//     → 워커마다 입력별 {from, how, ok, error, required}   (상태 inputs)
//     → 워커마다 넣어 줄 주소 {"<입력>": {"worker": "<상대>"}} (run/inputs/<id>.json)
//
// 순서: 선언(declared) → 내보내는 워커가 하나뿐이면 자동(auto) → 프리셋에 적은 inputs(config) → 없음.
// 검사(선언·자동): 상대가 그 역할을 내보내나, 버전이 이어지나(같은 큰 버전, 상대 작은 버전 ≥ 사본),
// 쓰는 주제·명령·데이터가 상대 정의에 있나. 틀리면 그 입력만 오류(주소를 넣지 않음), 워커 기동은 막지 않는다.
// 통로를 만들거나 데이터를 나르지 않는다(이름을 풀고 검사할 뿐).
#pragma once

#include <map>
#include <set>
#include <string>

#include "json_util.hpp"

namespace pm {

struct UsesEntry {
    std::string role, version;
    std::set<std::string> topics, commands, data;
    bool required = false;
};

struct ProvidedRole {
    std::string version;
    std::set<std::string> topics, commands, data;
};

struct WorkerIo {
    std::map<std::string, UsesEntry> uses;         // 입력 이름 → 쓰는 정의
    std::map<std::string, ProvidedRole> provides;  // 역할 → 내보내는 정의
    Json config_inputs = Json::object();           // 프리셋(지금 설정)의 inputs
};

// get_io 답과 지금 설정(config)으로. 계약이 없으면 io에 null
WorkerIo worker_io_from(const Json& io, const Json& config);

// "1.2"와 "1.4" → true(같은 큰 버전, 상대가 같거나 새로움). 형식이 틀리면 false
bool version_compatible(const std::string& uses, const std::string& provided);

struct InputResolution {
    Json status = Json::object();  // 워커 → {입력 → {from, how, ok, error, required}}
    Json inject = Json::object();  // 워커 → {입력 → {"worker": 상대}}(선언·자동 중 검사 통과만)
    Json errors = Json::array();   // 선언 자체의 오류(없는 워커·입력, 같은 입력에 둘)
};

InputResolution resolve_connections(const Json& connections, const std::map<std::string, WorkerIo>& workers);

// connections 형식 검사([{"to": "<워커>.<입력>", "from": "<워커>"}]). 틀리면 오류 글
std::string connections_shape_error(const Json& connections);

}  // namespace pm
