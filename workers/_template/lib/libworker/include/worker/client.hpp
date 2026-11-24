// 소켓 클라이언트: 워커 명령 소켓(design.md 9.6)과 Manager API(6.2). 워커 CLI·시험·Manager API를 쓰는 워커용.
#pragma once

#include <chrono>
#include <string>

#include "nlohmann/json.hpp"

namespace worker {

// {"cmd": cmd, "args": args}를 보내고 응답 한 줄을 JSON으로 돌려준다.
// 연결·응답 실패는 {"ok": false, "error": {"code": "connect" | "timeout" | "bad_reply", ...}}.
nlohmann::json send_command(const std::string& socket_path, const std::string& cmd,
                            const nlohmann::json& args = nlohmann::json::object(),
                            std::chrono::milliseconds timeout = std::chrono::seconds(2));

// JSON 한 줄을 보내고 응답 한 줄을 받는다(요청 1건 = 연결 1개). 실패 형식은 send_command와 같다
nlohmann::json send_request(const std::string& socket_path, const nlohmann::json& request,
                            std::chrono::milliseconds timeout = std::chrono::seconds(2));

// Manager API 호출(워커 선언 "manager_api": true인 워커만, design.md 6.5). 소켓은 Manager가 넘긴 환경 변수
// WORKSPACE_MANAGER_SOCKET(그 워커에게만 있다). 없으면 {"ok": false, "error": {"code": "no_manager_api"}}.
// 예: manager_call({{"op", "start"}, {"id", "siyi"}}), manager_call({{"op", "list"}})
// 읽기·쓰기 op 모두 된다. 자기 자신(자기 워커)은 제어할 수 없다(forbidden)
nlohmann::json manager_call(const nlohmann::json& request,
                            std::chrono::milliseconds timeout = std::chrono::seconds(2));

}  // namespace worker
