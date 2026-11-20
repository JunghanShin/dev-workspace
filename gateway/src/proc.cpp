#include "proc.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include "sys.hpp"

extern char** environ;

namespace gw {

namespace {

std::vector<std::string> make_env(const std::vector<std::string>& extra) {
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string kv = *e;
        const std::string key = kv.substr(0, kv.find('='));
        bool replaced = false;
        for (const auto& x : extra) replaced = replaced || x.substr(0, x.find('=')) == key;
        if (!replaced) env.push_back(kv);
    }
    for (const auto& x : extra) env.push_back(x);
    return env;
}

std::vector<char*> cvec(std::vector<std::string>& v) {
    std::vector<char*> out;
    for (auto& s : v) out.push_back(&s[0]);
    out.push_back(nullptr);
    return out;
}

int decode_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return -1;
}

// argv·env 준비 + posix_spawnp. fds: 자식의 1·2번에 이을 쓰기 끝
pid_t spawn(const std::vector<std::string>& argv_in, const std::vector<std::string>& env_extra, int out_w, int err_w,
            bool new_session, int& err) {
    std::vector<std::string> argv = argv_in, env = make_env(env_extra);
    auto av = cvec(argv), ev = cvec(env);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out_w, 1);
    posix_spawn_file_actions_adddup2(&fa, err_w, 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    // 부모(게이트웨이)가 막아 둔 신호를 자식은 풀고 시작
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&at, &none);
    sigset_t dfl;
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGPIPE);
    sigaddset(&dfl, SIGTERM);
    sigaddset(&dfl, SIGINT);
    posix_spawnattr_setsigdefault(&at, &dfl);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (new_session) flags |= POSIX_SPAWN_SETSID;
    posix_spawnattr_setflags(&at, flags);
    pid_t pid = -1;
    err = posix_spawnp(&pid, av[0], &fa, &at, av.data(), ev.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    return err == 0 ? pid : -1;
}

}  // namespace

RunResult run_capture(const std::vector<std::string>& argv, double timeout_s, const std::vector<std::string>& env_extra) {
    RunResult r;
    int po[2], pe[2];
    if (::pipe2(po, O_CLOEXEC) != 0) {
        r.spawn_err = errno;
        return r;
    }
    if (::pipe2(pe, O_CLOEXEC) != 0) {
        r.spawn_err = errno;
        ::close(po[0]);
        ::close(po[1]);
        return r;
    }
    int err = 0;
    const pid_t pid = spawn(argv, env_extra, po[1], pe[1], false, err);
    ::close(po[1]);
    ::close(pe[1]);
    if (pid < 0) {
        r.spawn_err = err;
        ::close(po[0]);
        ::close(pe[0]);
        return r;
    }
    r.started = true;
    const double deadline = mono_now() + timeout_s;
    int fds[2] = {po[0], pe[0]};
    std::string* bufs[2] = {&r.out, &r.err};
    bool open[2] = {true, true};
    while ((open[0] || open[1]) && !r.timed_out) {
        const double left = deadline - mono_now();
        if (left <= 0) {
            r.timed_out = true;
            break;
        }
        pollfd p[2];
        int n = 0, map[2];
        for (int i = 0; i < 2; ++i)
            if (open[i]) {
                p[n] = {fds[i], POLLIN, 0};
                map[n++] = i;
            }
        const int pr = ::poll(p, static_cast<nfds_t>(n), static_cast<int>(left * 1000) + 1);
        if (pr < 0 && errno == EINTR) continue;
        for (int k = 0; k < n; ++k) {
            if (!(p[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[65536];
            const ssize_t got = ::read(p[k].fd, buf, sizeof buf);
            if (got > 0) bufs[map[k]]->append(buf, static_cast<size_t>(got));
            else if (got == 0 || errno != EINTR) open[map[k]] = false;
        }
    }
    ::close(po[0]);
    ::close(pe[0]);
    // 출력이 닫힌 뒤에도 남은 시간 안에서만 기다린다
    while (!r.timed_out) {
        int code;
        if (try_reap(pid, code)) {
            r.code = code;
            return r;
        }
        if (mono_now() >= deadline) {
            r.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::kill(pid, SIGKILL);
    int st;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    r.code = decode_status(st);
    return r;
}

Child spawn_stream(const std::vector<std::string>& argv, const std::vector<std::string>& env_extra, bool new_session) {
    Child c;
    int p[2];
    if (::pipe2(p, O_CLOEXEC) != 0) {
        c.err = errno;
        return c;
    }
    c.pid = spawn(argv, env_extra, p[1], p[1], new_session, c.err);
    ::close(p[1]);
    if (c.pid < 0) {
        ::close(p[0]);
        return c;
    }
    c.fd = p[0];
    return c;
}

bool try_reap(pid_t pid, int& code) {
    int st = 0;
    const pid_t r = ::waitpid(pid, &st, WNOHANG);
    if (r == pid) {
        code = decode_status(st);
        return true;
    }
    if (r < 0 && errno == ECHILD) {
        code = -1;
        return true;
    }
    return false;
}

}  // namespace gw
