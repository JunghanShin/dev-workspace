// 역할 정의(io/*.json)의 스키마 부분집합으로 값을 검사한다(워크스페이스 dev/io_contract.py validate와 같은 규칙).
// type(배열 가능) · enum · minimum/maximum/exclusive* · required · properties · additionalProperties(기본 false) ·
// items · $ref(#/definitions/<이름>). 받은 칸 실행 중 검사(docs/input-check.md, libworker 0.17)가 쓴다.
#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace worker {

// v를 스키마 s로 검사, 틀린 곳을 errs에 "경로: 이유"로 더한다(max_errs개까지)
void validate_schema(const nlohmann::json& v, const nlohmann::json& s, const nlohmann::json& defs, const std::string& path,
                     std::vector<std::string>& errs, size_t max_errs = 4);

// 이름 자리(<…>)가 있는 정의 이름과 맞나("stream_<송출>_fps" ↔ "stream_main_fps"). 자리는 [a-z0-9_]+
bool name_matches(const std::string& pattern, const std::string& name);

}  // namespace worker
