#include "worker/child.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

namespace worker {

pid_t spawn(const std::vector<std::string>& cmd, int in, int out, int fd3) {
    if (cmd.empty()) return -1;
    std::vector<char*> argv;
    for (const auto& a : cmd) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid != 0) return pid;
    signal(SIGPIPE, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    const int devnull = open("/dev/null", O_RDWR);
    if (fd3 >= 0) dup2(fd3, 3);  // dup2는 CLOEXEC를 지운다
    dup2(in >= 0 ? in : devnull, STDIN_FILENO);
    dup2(out >= 0 ? out : devnull, STDOUT_FILENO);
    execvp(argv[0], argv.data());
    std::fprintf(stderr, "cannot run %s: %s\n", argv[0], std::strerror(errno));
    _exit(127);
}

void kill_wait(pid_t& pid, std::chrono::milliseconds grace) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    const auto end = std::chrono::steady_clock::now() + grace;
    while (std::chrono::steady_clock::now() < end) {
        if (waitpid(pid, nullptr, WNOHANG) != 0) {
            pid = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    pid = -1;
}

std::string exit_text(const std::string& name, int status) {
    if (WIFEXITED(status)) return name + " exited " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return name + " killed by " + strsignal(WTERMSIG(status));
    return name + " ended";
}

}  // namespace worker
