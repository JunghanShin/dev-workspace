// 읽기 전용 보드 조회 (design.md 6.9, Python workspace_rest/reads.py·profile.py). 모두 GET, 보드를 바꾸지 않는다.
// 경로는 고정이고 워커 id·숫자만 받는다(임의 파일 경로·명령 없음). 외부 명령(journalctl, ps, git)은 시간 상한.
//
//   /logs/workers/{id}?lines=200   워커 로그 끝부분                 {"lines": [...]}
//   /logs/service?lines=200        workspace 서비스 로그(journal)    {"lines": [...]}
//   /records/commands?lines=200    명령 기록(logger 최근 파일 3개)    {"lines": [JSON 문자열...]}
//   /system/processes?lines=12     CPU 순 프로세스                  {"processes": [...]}
//   /system/power_modes            nvpmodel 모드 이름               {"modes": {...}}
//   /system/workspace              워크스페이스·프로필(비밀값 가림)
//   /workers/{id}/tasks?pid=N      워커 프로세스 그룹의 프로세스·스레드
//   /history/{id}?minutes=&points=&keys=&csv=1   지난 지표
#pragma once

#include <mutex>
#include <stdexcept>
#include <string>

#include "routes.hpp"
#include "text.hpp"

namespace gw {

class BadRequest : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ReadResult {
    int status = 200;
    Json body;
};

class Reads {
public:
    Reads(std::string workspace, std::string data_dir) : workspace_(std::move(workspace)), data_(std::move(data_dir)) {}
    // name: 읽기 이름, id: 경로의 워커 id(없으면 has_id=false). 잘못된 값은 BadRequest, 그 밖의 실패는 runtime_error
    ReadResult handle(const std::string& name, const std::string& id, bool has_id, const Params& params);

private:
    Json workspace_info();
    Json git_info();
    Json group_tasks(long long pgid);

    const std::string workspace_, data_;
    std::mutex git_mu_;  // git 캐시(잠금 안에서는 읽고 쓰기만, git 실행은 밖에서)
    double git_t_ = -1e18;
    Json git_v_;
};

}  // namespace gw
