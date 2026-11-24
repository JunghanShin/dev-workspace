// 명령하는 곳 검사 (워크스페이스 docs/command-path.md 4.2·5장 ②). 워커 명령 소켓에 붙은 상대가 누구인지(커널이
// 알려 준 pid) process-manager가 만든 허용 목록(peers.json)으로 알아보고, 그 명령을 보내도 되는지 본다.
//
//   - 허용 목록 파일 위치는 환경 변수 WORKSPACE_PEERS_FILE(process-manager가 띄울 때 넣음). 없으면(단독 실행) 보지 않는다
//   - 판단은 없다: 표대로 넣고 뺀다. guard "warn"이면 어긋나도 실행하고 기록만, "enforce"면 거절
//   - 명령이 올 때 파일 바뀐 시각을 보고 바뀌었으면 다시 읽는다(워커를 다시 시작하지 않음)
// 스레드 안전하지 않다(호스트 루프에서만).
#pragma once

#include <map>
#include <set>
#include <string>

#include "nlohmann/json.hpp"

namespace worker {

class CommandGuard {
public:
    struct Decision {
        bool run = true;       // 실행할지(enforce에서 거절이면 false)
        bool violation = false;// 허용 목록 밖(warn이면 run=true인 채로 기록)
        bool read = false;     // 읽기 명령(get_status, 계약 "read": true)
        std::string from;      // 보낸 곳(screen, remote:이름, 워커 id, unknown:<pid>)
        std::string reason;    // 위반 이유(기록용)
        bool trusted = false;  // 믿는 곳(게이트웨이·명령 입구 매니저): 봉투의 from·origin을 쓴다
    };

    // path가 비면 검사하지 않는다(단독 실행). worker_id는 표에서 자기 칸을 찾을 때
    CommandGuard(std::string worker_id, std::string path);

    // pid: 상대 프로세스(SO_PEERCRED, 모르면 0). claimed_from: 명령의 "from"(게이트웨이처럼 믿는 곳만 씀)
    Decision check(int pid, const std::string& cmd, const std::string& claimed_from);

    bool enabled() const { return !path_.empty(); }
    const std::string& mode() const { return guard_; }  // off·warn·enforce(표가 없으면 warn)

private:
    struct Peer {
        int pid = 0, pgid = 0;
        bool trusted_from = false;
    };
    void reload();

    std::string id_, path_;
    long long mtime_ns_ = -1;
    bool loaded_ = false;
    std::string guard_ = "warn";
    std::map<std::string, Peer> peers_;
    std::set<std::string> read_;
    std::map<std::string, std::set<std::string>> allow_;  // 곳 → 허용 명령
    std::set<std::string> allow_all_;                     // "*"인 곳
};

}  // namespace worker
