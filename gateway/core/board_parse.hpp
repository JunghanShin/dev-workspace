// 보드 읽기 글 해석 (Python workspace_rest/reads.py·productbuild.py). 명령 실행·파일 읽기는 부르는 쪽이 한다.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "text.hpp"

namespace gw {

// "ps -eo pid,comm,%cpu,%mem,rss --no-headers" 출력 앞 n줄 → [{pid, name, cpu, mem, rss_kb}]. 숫자가 틀리면 invalid_argument
Json parse_ps(const std::string& out, long long n);
// /etc/nvpmodel.conf → {"0": "MAXN_SUPER", ...}
Json parse_nvpmodel(const std::string& text);
// /proc/<pid>/stat → comm, 3번째 칸(state)부터의 칸들. 형식이 틀리면 false
bool parse_proc_stat(const std::string& data, std::string& comm, std::vector<std::string>& fields);
// sched 정책 번호 → 이름
std::string policy_name(long long policy);

// 제품 빌드 상태(productbuild.Job.st·log). 잠금은 부르는 쪽
struct BuildState {
    Json st = Json{{"state", "idle"}};
    std::vector<std::string> log;
    // 자식 출력 한 줄: "##PROGRESS <0~100> <글>", "##RESULT <JSON>", "##ERROR <글>", 그 밖은 기록
    void apply_line(const std::string& line);
    // 자식이 끝남(code: 종료 코드, 신호면 -신호)
    void finish(int code, double now);
};
constexpr size_t kBuildLogLines = 300;

}  // namespace gw
