// Manager 설정·워커 type 레지스트리 읽기 (Python settings.py). 시작할 때 한 번.
//
// 기본값은 design.md 14장. 경로는 데이터 폴더(WORKSPACE_DATA) 기준. 깨진 파일은 기본값·그 파일만 건너뜀.
#pragma once

#include <set>
#include <string>

#include "json_util.hpp"

namespace pm {

// 기본값 + 설정 파일. 경로 칸이 없고 data_dir도 없으면 예외(std::runtime_error)
Json load_settings(const std::string& path, const std::string& data_dir);
// types_file + types_dir/*.json. 워커 폴더 조각이면 _worker_id·_conf_dir
Json load_types(const std::string& path, const std::string& dir, const Json& worker_defaults, const std::string& root);
// <root>/*/worker-types.json(기본 탑재)을 더한다(같은 이름이 있으면 그것을 씀)
void add_builtin_types(Json& types, const Json& worker_defaults, const std::string& root);
// type마다 "requires" 도구가 다 있는지 → _requires_met
void mark_requirements(Json& types);
// "manager_api": true인 워커 id
std::set<std::string> manager_api_workers(const Json& types);
// 모듈 운용 매니저(kind manager + module_control true): 쓰기 소켓, 운용 모드에서도 워커 start·stop
std::set<std::string> module_controls(const Json& types);
// 명령 입구 매니저(kind manager + command_entry true): 허용 목록에서 게이트웨이처럼
std::set<std::string> command_entries(const Json& types);
// 매니저 읽기 전용 클라이언트(kind manager, manager_api가 아닌 것)
std::set<std::string> manager_clients(const Json& types, const std::set<std::string>& api_workers);
// 절대 경로로, 정리해서(os.path.abspath)
std::string abspath(const std::string& p);

}  // namespace pm
