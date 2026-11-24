// 실행 파일에 넣은 입출력 정의(워크스페이스 design.md 6.6.7.1). `<워커> --io`가 이것을 출력한다.
//
// 넣는 법: 워커 CMakeLists에서 worker_io(<실행 파일 대상> ${CMAKE_CURRENT_SOURCE_DIR}).
// 빌드가 워커 폴더의 io.json, io/*.json, io/uses/*.json, lib/*/io/*.json(include 조각, libworker의 공통 정의)을
// 생성 파일에 담고, 그 파일이 시작할 때 register_io_files로 등록한다. 실행 때 파일을 찾지 않는다(단독 실행 그대로).
//
// io_bundle()은 process-manager get_io와 같은 모양(id만 없음):
//   {"common": …, "self": …, "includes": {"<경로>": …}, "provides": {"<역할>": …}, "uses": {"<입력>": {…, "definition": <사본>|null}}}
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace worker {

using IoFiles = std::vector<std::pair<const char*, const char*>>;  // (워커 폴더 기준 경로, 파일 내용)

// 생성 파일이 부른다. 여러 번 부르면 뒤의 것이 같은 경로를 덮는다. 늘 true(정적 초기화용)
bool register_io_files(const IoFiles& files);
// 등록된 파일 수(0이면 worker_io를 안 부른 빌드)
size_t io_file_count();
// 등록된 파일로 정의 묶음을 만든다. 실패(io.json 없음, 깨진 JSON, 없는 조각)면 null과 *error
nlohmann::ordered_json io_bundle(std::string* error);

}  // namespace worker
