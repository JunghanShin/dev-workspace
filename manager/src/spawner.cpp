#include "spawner.hpp"

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "json_util.hpp"
#include "posix_fs.hpp"

namespace pm {

namespace {

std::string env_get(const std::vector<std::string>& env, const std::string& key) {
    const std::string p = key + "=";
    for (const auto& e : env)
        if (e.rfind(p, 0) == 0) return e.substr(p.size());
    return "";
}

// 실행 파일 후보: '/'가 있으면 그대로, 없으면 워커 환경의 PATH에서(Python subprocess와 같음)
std::vector<std::string> exec_paths(const std::string& name, const std::vector<std::string>& env) {
    if (name.find('/') != std::string::npos) return {name};
    std::string path = env_get(env, "PATH");
    if (path.empty()) path = "/bin:/usr/bin";
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find(':', i);
        if (j == std::string::npos) j = path.size();
        const std::string dir = path.substr(i, j - i);
        out.push_back((dir.empty() ? std::string(".") : dir) + "/" + name);
        i = j + 1;
    }
    return out;
}

void write_errno(int fd, int err) {
    [[maybe_unused]] ssize_t n = ::write(fd, &err, sizeof err);
}

}  // namespace

int PosixSpawner::spawn(const SpawnSpec& spec, std::string& err) {
    if (spec.argv.empty()) {
        err = "empty exec";
        return -1;
    }
    // ---- 부모에서 준비(fork 뒤에는 메모리를 잡지 않는다) ----
    std::string dir_err;
    const size_t slash = spec.log_path.rfind('/');
    if (slash != std::string::npos) mkdir_p(spec.log_path.substr(0, slash), &dir_err);
    const int logfd = ::open(spec.log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (logfd < 0) {
        err = errno_msg(errno, spec.log_path);
        return -1;
    }
    const int nullfd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    int pipefd[2];
    if (nullfd < 0 || ::pipe2(pipefd, O_CLOEXEC) != 0) {
        err = errno_msg(errno, "/dev/null");
        ::close(logfd);
        if (nullfd >= 0) ::close(nullfd);
        return -1;
    }
    std::vector<char*> argv, envp;
    for (const auto& a : spec.argv) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    for (const auto& e : spec.env) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    const std::vector<std::string> paths = exec_paths(spec.argv[0], spec.env);
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    for (int c : spec.cpus)
        if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &cpus);
    const bool has_nice = spec.nice.has_value();
    const int nice = spec.nice.value_or(0);
    const bool has_cpus = !spec.cpus.empty();
    const pid_t parent = ::getpid();
    sigset_t empty;
    sigemptyset(&empty);

    const pid_t pid = ::fork();
    if (pid < 0) {
        err = errno_msg(errno, spec.argv[0]);
        ::close(logfd);
        ::close(nullfd);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        // ---- 자식: 시스템 호출만 ----
        // Manager가 막아 둔 신호(signalfd용)를 풀고, 기본 동작으로
        for (int s : {SIGCHLD, SIGTERM, SIGINT, SIGHUP, SIGPIPE}) ::signal(s, SIG_DFL);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);
        ::setsid();  // 자기 세션·프로세스 그룹의 리더(워커가 띄운 자식까지 그룹으로 정리)
        // Manager가 systemd 밖에서 죽어도 워커가 고아로 남지 않게. 이미 죽었으면 바로 끝낸다
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != parent) ::_exit(127);
        // 우선순위·CPU: exec 전에 정해 워커의 모든 스레드가 물려받음. 실패해도 띄운다
        if (has_nice) ::setpriority(PRIO_PROCESS, 0, nice);
        if (has_cpus) ::sched_setaffinity(0, sizeof cpus, &cpus);
        ::dup2(nullfd, 0);
        ::dup2(logfd, 1);
        ::dup2(logfd, 2);
        int last = ENOENT;
        for (const auto& p : paths) {
            ::execve(p.c_str(), argv.data(), envp.data());
            last = errno;
            if (last != ENOENT && last != ENOTDIR) break;
        }
        write_errno(pipefd[1], last);
        ::_exit(127);
    }
    // ---- 부모: exec 성공이면 파이프가 닫혀(CLOEXEC) 0바이트, 실패면 errno ----
    ::close(pipefd[1]);
    ::close(logfd);
    ::close(nullfd);
    int child_err = 0;
    ssize_t n;
    do {
        n = ::read(pipefd[0], &child_err, sizeof child_err);
    } while (n < 0 && errno == EINTR);
    ::close(pipefd[0]);
    if (n == static_cast<ssize_t>(sizeof child_err)) {
        int st;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
        err = errno_msg(child_err, spec.argv[0]);
        return -1;
    }
    return pid;
}

void PosixSpawner::signal(int pid, int sig) {
    if (pid > 0) ::kill(pid, sig);
}

void PosixSpawner::kill_group(int pgid, int sig) {
    if (pgid > 0) ::killpg(pgid, sig);
}

bool PosixSpawner::executable(const std::string& path) { return ::access(path.c_str(), X_OK) == 0; }

}  // namespace pm
