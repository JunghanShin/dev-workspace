// 워커 설정 스키마 검사 (design.md 6.10). Python schema.py를 그대로 옮김(메시지도 같게).
//
// 스키마: {"groups": [{"section": "<모듈>"?, "fields": [칸, ...]}]}. section이 있으면 그 칸은 config["<모듈>"] 안.
// 스키마에 없는 키는 검사하지 않는다. 오류는 {경로: 메시지}.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "json_util.hpp"

namespace pm::schema {

// [(section 또는 null, 칸 목록)]. 같은 section의 묶음은 하나로
std::vector<std::pair<Json, std::vector<Json>>> sections_of(const Json& schema);
std::vector<Json> fields_of(const Json& schema);

// 스키마 자체의 형식 오류(워커 개발자용). 비어 있으면 정상
std::vector<std::string> check_schema(const Json& schema);
// 칸 목록으로 객체 하나 → {경로: 메시지}
Json check_object(const std::vector<Json>& fields, const Json& obj, const std::string& prefix = "");
// 설정 전체 → {경로: 메시지}. 스키마가 없으면 {}
Json check_config(const Json& schema, const Json& config);
// 다른 워커를 가리키는 값(format worker_id) → {경로: 워커 id}
Json worker_refs(const Json& schema, const Json& config);
// 같은 종류 워커끼리 겹치면 안 되는 값(칸에 "unique": true, 예: 장치 시리얼) → {경로: 값}. 빈 값·null은 뺀다
Json unique_values(const Json& schema, const Json& config);

}  // namespace pm::schema
