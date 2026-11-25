// 워크스페이스 설정 규칙 (ui/workspace-screen.md 2장, docs/gateway.md 3장 "설정"). 파일 읽기·쓰기는 부르는 쪽(src/server)이 한다.
//
//   정의: install/config/workspace-settings.json {"version", "groups": [{"title", "fields": [{key, label, type, apply, default, ...}]}]}
//         type: string | number | integer | enum, apply: live(바로) | restart(다시 시작)
//   값:   <데이터>/config/workspace.env 의 KEY=값 줄(workspace-start.sh read_kv가 읽는 모양)
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "text.hpp"

namespace gw {

// 정의 글 → 정의(모양 검사). 틀리면 false와 이유
bool settings_parse_def(const std::string& text, Json& def, std::string& err);
// 정의에서 키의 칸(없으면 nullptr)
const Json* settings_field(const Json& def, const std::string& key);
// 파일 글(또는 env 값) → 칸 형식대로의 JSON 값(number→실수, integer→정수). 형식대로 못 읽으면 글 그대로
Json settings_typed(const Json& field, const std::string& raw);
// 칸 기본값을 파일 글 모양으로("3", "INFO", "")
std::string settings_default_text(const Json& field);

// 바꿀 것 하나: 값이 없으면(nullopt) 지움(기본값으로)
using EnvChanges = std::vector<std::pair<std::string, std::optional<std::string>>>;

// 본문 {"KEY": 값, ...} 검사. 맞으면 true와 changes(본문 순서, 파일 글 모양). 틀리면 false와 칸별 이유(fields),
// 본문 모양 자체가 틀리면 fields 없이 err. null·""은 지움
bool settings_check(const Json& def, const Json& body, EnvChanges& changes, Json& fields, std::string& err);

// workspace.env 글에서 그 키 줄만 바꾼다. 주석·순서·다른 키 그대로.
//   있는 줄(주석 아님)  → 바꿈(같은 키가 더 있으면 뒤의 것은 뺌)
//   "#KEY=" 줄만 있음  → 그 줄을 풀어 바꿈
//   없음               → 끝에 더함
//   지움               → 그 줄을 "#KEY=옛값"으로(주석 줄이 따로 있으면 그냥 뺌)
std::string env_edit(const std::string& text, const EnvChanges& changes);
// 주석 아닌 KEY=값(같은 키면 뒤의 것, read_kv와 같음: # 뒤는 버림, 끝 공백·앞뒤 " 뗌)
std::map<std::string, std::string> env_active(const std::string& text);

}  // namespace gw
