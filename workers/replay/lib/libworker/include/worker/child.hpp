// 자식 프로세스(ffmpeg, gst-launch 등) 도우미.
//
// - 자식은 워커와 같은 프로세스 그룹에 둔다. 워커가 어떻게 끝나든 Manager의 그룹 정리(design.md 9.3)가
//   자식까지 끝낸다. 단독 실행에서는 워커의 종료 절차가 끝낸다
// - 워커가 무시하는 시그널(SIGPIPE, SIGHUP)은 exec 뒤에도 무시되므로 기본으로 돌린다
// - 커널 타이머(워치독)는 fork에서 물려받지 않는다
#pragma once

#include <sys/types.h>

#include <chrono>
#include <string>
#include <vector>

namespace worker {

// fork + exec(PATH에서 찾음). in/out: 자식의 stdin/stdout(-1이면 /dev/null). fd3: 자식의 fd 3으로
// 넘길 fd(-1이면 없음). stderr는 워커 로그 그대로. 실패하면 -1.
pid_t spawn(const std::vector<std::string>& cmd, int in = -1, int out = -1, int fd3 = -1);

// SIGTERM 후 grace 안에 끝나지 않으면 SIGKILL, 회수한다(최대 grace 동안 블로킹). pid는 -1이 된다.
void kill_wait(pid_t& pid, std::chrono::milliseconds grace = std::chrono::seconds(1));

// waitpid status → "ffmpeg exited 1", "gst-launch killed by Killed"
std::string exit_text(const std::string& name, int status);

}  // namespace worker
