// 워크스페이스·프로필 정보 규칙 (Python workspace_rest/profile.py). 파일 읽기는 부르는 쪽(src/reads)이 한다.
//
// 비밀값 가림(design.md 10.3): 키 이름에 pass·token·secret·api_key·private_key가 들어간 값은 "••••"
#pragma once

#include <optional>
#include <string>

#include "text.hpp"

namespace gw {

extern const char* const kMask;
constexpr size_t kMaxText = 64 * 1024;  // 파일 내용은 글자 64 Ki까지

bool secret_name(const std::string& key);
// 줄마다 "<키>: 값" / "<키> = 값"에서 비밀 키의 값을 가린다(profile._SECRET_LINE)
std::string mask_text(const std::string& text);
// 기본 workspace.env → [{key, default, example, desc, group}]
Json env_doc(const std::string& text);
// KEY=VALUE(주석 아닌 줄)
Json env_values(const std::string& text);
// 프로필로 바꾸는 설정 파일 이름인가(README·*.md·숨김·worker-types.json·product.json·workspace-settings.json(설정 탭 칸 정의) 제외). 파일인지는 부르는 쪽이 봄
bool profile_file_name(const std::string& name);
// 보드 파일 출처: missing | profile | changed | default | unknown
std::string file_origin(const std::optional<std::string>& board, const std::optional<std::string>& prof,
                        const std::optional<std::string>& dflt, bool has_src);
// 마지막 단계: env 항목에 value·in_profile을 채우고 비밀값을 가린다
void env_finish(Json& env, const Json& board_values, const Json& prof_values);

}  // namespace gw
