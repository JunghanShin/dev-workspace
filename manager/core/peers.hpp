// 명령하는 곳 허용 목록 (docs/command-path.md 4장). 시스템 호출 없이 표만 만든다(제어 루프에서 부름, test_core).
//
//   연결 선언 commanders {"<워커>[.<역할>]": ["<워커·매니저 id>", ...]}  +  워커 계약(get_io)  +  모드  +  프로세스
//     → check_commanders(): 항목마다 {ok, error, warning}
//     → build_peers():      run/peers.json 내용(libworker가 읽는 표)
//
// 판단은 없다: 무엇을 허용할지는 연결 선언과 계약이 정한다. 화면(게이트웨이)은 늘 "*"(모드 검사는 게이트웨이가 함).
#pragma once

#include <map>
#include <set>
#include <string>

#include "json_util.hpp"

namespace pm {

// 워커 하나의 명령 표(계약에서)
struct WorkerCommands {
    std::set<std::string> read;                          // get_status + "read": true
    std::set<std::string> operate;                       // "operate": true(워커 전용이지만 운용 중에도 쓰는 조작)
    std::map<std::string, std::set<std::string>> roles;  // 역할 → 그 역할 정의의 명령
    std::set<std::string> uses;                          // 이 워커가 쓰는 역할(io.json uses의 from) — 명령하는 곳 검사용
};

// get_io 답(design.md 6.6.7.1)에서 명령 표를 만든다. 계약이 없으면(get_io 실패) 빈 표를 넘기면 된다
WorkerCommands commands_from_io(const Json& io);

// commanders 형식 검사(파일 전체). 틀리면 오류 글, 맞으면 빈 글
std::string commanders_shape_error(const Json& commanders);

// 항목마다 검사: 워커가 있나, 그 역할을 내보내나, 명령하는 곳이 있는 워커인가(매니저 id는 나중), 그 역할을 쓴다고 적었나(경고)
// workers: 등록된 워커 id → 명령 표(계약 없는 워커도 빈 표로 넣음)
Json check_commanders(const Json& commanders, const std::map<std::string, WorkerCommands>& workers);

struct PeerProcess {
    std::string id;   // 명령하는 곳(워커 id, "gateway")
    int pid = 0;      // 게이트웨이처럼 프로세스 하나
    int pgid = 0;     // 워커처럼 프로세스 그룹(process-manager가 새 그룹으로 띄움)
    bool trusted_from = false;
};

// peers.json 내용. written_at_ms는 넣지 않는다(바뀌었는지 비교하려고. 쓸 때 더함)
// entry: 명령 입구 매니저 id(설치돼 있고 역할 mission을 내보낼 때, managers/mission/external-link.md). 있으면
//   "entry": {"id"}, 다른 워커에 대한 commanders 항목은 allow가 아니라 via(입구가 검사)로, 그 명령하는 곳은 입구의
//   역할 mission 명령을 allow에 자동으로
Json build_peers(const Json& commanders, const std::map<std::string, WorkerCommands>& workers,
                 const std::vector<PeerProcess>& procs, const std::string& mode, const std::string& guard,
                 const Json& boot_id, const std::string& entry = "");

}  // namespace pm
