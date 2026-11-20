#include "sys.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "ui_rules.hpp"

namespace gw {

double mono_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double wall_now() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec / 1e9;
}

bool is_file(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_dir(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool path_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = ::opendir(path.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") out.push_back(n);
    }
    ::closedir(d);
    return out;
}

std::optional<std::string> read_file(const std::string& path, size_t max, int* err) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = errno;
        return std::nullopt;
    }
    std::string out;
    char buf[65536];
    while (out.size() < max) {
        const ssize_t n = ::read(fd, buf, std::min(sizeof buf, max - out.size()));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (err) *err = errno;
            ::close(fd);
            return std::nullopt;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return out;
}

std::optional<std::string> read_text(const std::string& path, size_t chars) {
    // 글자 하나는 4바이트 이하, \r\n은 2바이트에 1글자 → (chars+1)*4 바이트면 chars+1글자를 넘게 읽는다
    auto raw = read_file(path, (chars + 1) * 4 + 2);
    if (!raw) return std::nullopt;
    return utf8_prefix(universal_newlines(utf8_replace(*raw)), chars + 1);
}

bool for_each_line(const std::string& path, const std::function<bool(const std::string&)>& fn) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::string cur;
    bool cr = false;  // 앞 조각이 \r로 끝남(다음이 \n이면 같은 줄바꿈)
    char buf[65536];
    bool go = true;
    while (go) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n && go; ++i) {
            const char c = buf[i];
            if (cr && c == '\n') {
                cr = false;
                continue;
            }
            cr = false;
            if (c == '\n' || c == '\r') {
                cr = c == '\r';
                go = fn(utf8_replace(cur));
                cur.clear();
            } else {
                cur += c;
            }
        }
    }
    if (go && !cur.empty()) fn(utf8_replace(cur));
    ::close(fd);
    return true;
}

double mtime_of(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return -1;
    return static_cast<double>(st.st_mtim.tv_sec) + st.st_mtim.tv_nsec / 1e9;
}

long long size_of(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<long long>(st.st_size) : -1;
}

std::string abspath(const std::string& path) {
    if (!path.empty() && path[0] == '/') return normpath(path);
    char cwd[4096];
    const std::string base = ::getcwd(cwd, sizeof cwd) ? cwd : "/";
    return normpath(path_join(base, path));
}

std::string env_or(const char* key, const std::string& dflt) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : dflt;
}

// ---- Unix 소켓 ----

namespace {
int wait_fd(int fd, short events, double deadline) {
    while (true) {
        const double left = deadline - mono_now();
        if (left <= 0) return 0;
        pollfd p{fd, events, 0};
        const int r = ::poll(&p, 1, static_cast<int>(std::ceil(left * 1000)));
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}
}  // namespace

int connect_unix(const std::string& path, double timeout_s, int& err, bool& timed_out) {
    err = 0;
    timed_out = false;
    sockaddr_un addr{};
    if (path.size() >= sizeof addr.sun_path) {
        err = ENAMETOOLONG;
        return -1;
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        err = errno;
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) return fd;
    if (errno != EINPROGRESS && errno != EAGAIN) {
        err = errno;
        ::close(fd);
        return -1;
    }
    // 받는 쪽 대기열이 가득: 자리가 날 때까지(상한까지) 다시 시도
    const double deadline = mono_now() + timeout_s;
    while (true) {
        if (wait_fd(fd, POLLOUT, deadline) <= 0) {
            timed_out = true;
            ::close(fd);
            return -1;
        }
        int soerr = 0;
        socklen_t len = sizeof soerr;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr == 0) {
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 || errno == EISCONN) return fd;
            if (errno == EAGAIN || errno == EINPROGRESS || errno == EALREADY) {
                ::usleep(10000);  // Unix 소켓은 대기열이 비었는지 poll로 알 수 없어 잠깐 쉬고 다시
                continue;
            }
            soerr = errno;
        }
        err = soerr;
        ::close(fd);
        return -1;
    }
}

Io send_all(int fd, const std::string& data, double deadline, int& err) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(fd, POLLOUT, deadline) <= 0) return Io::Timeout;
            continue;
        }
        err = errno;
        return Io::Error;
    }
    return Io::Ok;
}

Io read_line(int fd, std::string& buf, size_t max, double deadline, int& err) {
    while (buf.find('\n') == std::string::npos) {
        char tmp[4096];
        const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n > 0) {
            buf.append(tmp, static_cast<size_t>(n));
            if (buf.size() > max) return Io::TooLarge;
            continue;
        }
        if (n == 0) return Io::Closed;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (wait_fd(fd, POLLIN, deadline) <= 0) return Io::Timeout;
            continue;
        }
        err = errno;
        return Io::Error;
    }
    return Io::Ok;
}

namespace {
struct FdGuard {
    int fd;
    ~FdGuard() {
        if (fd >= 0) ::close(fd);
    }
};

std::string os_error(int err) {
    return err == ENAMETOOLONG ? "AF_UNIX path too long" : errno_text(err);
}
}  // namespace

Json ManagerClient::call(const Json& request) const {
    int err;
    bool timed_out;
    FdGuard g{connect_unix(path_, connect_s_, err, timed_out)};
    if (g.fd < 0) throw ManagerUnavailable("connect: " + (timed_out ? std::string("timed out") : os_error(err)));
    const double deadline = mono_now() + response_s_;
    switch (send_all(g.fd, compact(request) + "\n", deadline, err)) {
        case Io::Ok: break;
        case Io::Timeout: throw ManagerUnavailable("response timeout");
        default: throw ManagerUnavailable(errno_text(err));
    }
    std::string buf;
    switch (read_line(g.fd, buf, max_, deadline, err)) {
        case Io::Ok: break;
        case Io::Timeout: throw ManagerUnavailable("response timeout");
        case Io::Closed: throw ManagerUnavailable("connection closed");
        case Io::TooLarge: throw ManagerUnavailable("response too large");
        case Io::Error: throw ManagerUnavailable(errno_text(err));
    }
    std::string perr;
    auto j = parse_json(buf.substr(0, buf.find('\n')), &perr);
    if (!j) throw ManagerUnavailable("bad response: " + perr);
    if (!j->is_object()) throw ManagerUnavailable("bad response: not an object");
    return *j;
}

CommandResult worker_command(const std::string& path, const Json& request, double connect_s, double response_s) {
    CommandResult r;
    int err;
    bool timed_out;
    FdGuard g{connect_unix(path, connect_s, err, timed_out)};
    if (g.fd < 0) {
        if (timed_out) r.kind = CommandResult::Timeout;
        else if (err == ENOENT) r.kind = CommandResult::NoSocket;
        else r.message = os_error(err);
        return r;
    }
    // 응답 상한은 전체 시간(Python은 recv마다, 조금씩 흘리는 워커도 상한 안에 끊음)
    const double deadline = mono_now() + response_s;
    Io io = send_all(g.fd, compact(request) + "\n", deadline, err);
    std::string buf;
    if (io == Io::Ok) io = read_line(g.fd, buf, 65536, deadline, err);
    if (io == Io::Timeout) {
        r.kind = CommandResult::Timeout;
        return r;
    }
    if (io == Io::TooLarge) {
        r.message = "response too large";
        return r;
    }
    if (io == Io::Error) {
        r.message = errno_text(err);
        return r;
    }
    // Ok 또는 Closed(줄바꿈 없이 끝남): 받은 것으로 해석
    std::string perr;
    auto j = parse_json(buf.substr(0, buf.find('\n')), &perr);
    if (!j) {
        r.message = buf.empty() ? "Expecting value: line 1 column 1 (char 0)" : perr;
        return r;
    }
    if (!j->is_object()) {
        r.message = "reply is not an object";
        return r;
    }
    r.kind = CommandResult::Ok;
    r.reply = *j;
    return r;
}

}  // namespace gw
