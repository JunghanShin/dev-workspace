// 자식 프로세스: 보드 읽기(journalctl, ps, git)와 제품 빌드(python3 dev/build_product.py). 모든 기다림에 시간 상한.
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace gw {

struct RunResult {
    bool started = false;
    int spawn_err = 0;      // 못 띄웠을 때 errno
    bool timed_out = false; // 상한을 넘겨 죽임
    int code = 0;           // 종료 코드(신호면 -신호)
    std::string out, err;   // 받은 그대로(바이트)
};

// argv[0]은 PATH에서 찾는다. env_extra: "KEY=VALUE"(같은 키는 덮음)
RunResult run_capture(const std::vector<std::string>& argv, double timeout_s,
                      const std::vector<std::string>& env_extra = {});

// 출력을 계속 읽을 자식(stdout·stderr를 한 파이프로). new_session이면 새 세션(취소할 때 그룹째 신호)
struct Child {
    pid_t pid = -1;
    int fd = -1;
    int err = 0;
};
Child spawn_stream(const std::vector<std::string>& argv, const std::vector<std::string>& env_extra, bool new_session);
// 끝났으면 종료 코드(신호면 -신호)를 code에 넣고 true. 아직이면 false(기다리지 않음)
bool try_reap(pid_t pid, int& code);

}  // namespace gw
