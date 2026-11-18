#include "worker/command_server.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

namespace worker {

namespace {

constexpr size_t kMaxLine = 64 * 1024;
constexpr auto kConnTimeout = std::chrono::seconds(2);    // 연결 뒤 요청 한 줄을 받을 때까지(느리거나 멈춘 연결)
constexpr auto kReplyTimeout = std::chrono::seconds(60);  // 요청을 받은 뒤 응답까지 안전 상한(모듈이 응답을 잃었을 때)

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// mkdir -p
bool make_dirs(const std::string& dir) {
    if (dir.empty()) return true;
    size_t pos = 0;
    while ((pos = dir.find('/', pos + 1)) != std::string::npos) {
        const std::string part = dir.substr(0, pos);
        if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    return mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST;
}

}  // namespace

CommandServer::~CommandServer() { close(); }

bool CommandServer::open(const std::string& path, std::string* error) {
    sockaddr_un addr{};
    if (path.size() >= sizeof(addr.sun_path)) {
        *error = "command socket path too long: " + path;
        return false;
    }
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && !make_dirs(path.substr(0, slash))) {
        *error = "cannot create folder for " + path + ": " + std::strerror(errno);
        return false;
    }
    ::unlink(path.c_str());
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        *error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const mode_t old_mask = umask(0177);  // 0600
    const int rc = bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    umask(old_mask);
    if (rc != 0 || listen(listen_fd_, 16) != 0) {
        *error = "bind " + path + ": " + std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    set_nonblocking(listen_fd_);
    path_ = path;
    return true;
}

void CommandServer::close() {
    for (auto& item : conns_) ::close(item.second.fd);
    conns_.clear();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(path_.c_str());
    }
}

void CommandServer::add_pollfds(std::vector<pollfd>& fds) const {
    if (listen_fd_ >= 0) fds.push_back({listen_fd_, POLLIN, 0});
    for (const auto& item : conns_) {
        if (!item.second.answered) fds.push_back({item.second.fd, POLLIN, 0});
    }
}

void CommandServer::on_poll(const std::vector<pollfd>& fds) {
    std::vector<int> ready;
    for (const auto& p : fds) {
        if (!p.revents) continue;
        if (p.fd == listen_fd_) {
            accept_all();
            continue;
        }
        for (const auto& item : conns_) {
            if (item.second.fd == p.fd) ready.push_back(item.first);
        }
    }
    for (int id : ready) read_conn(id);
}

void CommandServer::on_timer(Clock::time_point now) {
    // 요청을 받기 전에만 짧게 끊는다. 받은 뒤에는 모듈이 나중에 답할 수 있다(장치 응답 대기 등).
    // 예전(0.9.0)에는 받은 뒤에도 연결 2 s에 끊어, 2 s 넘게 걸리는 명령은 응답 대신 빈 연결 끊김을 받았다
    std::vector<int> stale, lost;
    for (const auto& item : conns_) {
        const Conn& c = item.second;
        if (!c.answered && now - c.opened > kConnTimeout) stale.push_back(item.first);
        if (c.answered && now - c.requested > kReplyTimeout) lost.push_back(item.first);
    }
    for (int id : stale) drop(id);
    for (int id : lost) reply(id, R"({"ok":false,"error":{"code":"internal","message":"no reply within 60 s"}})");
}

void CommandServer::reply(int conn, const std::string& line) {
    const auto it = conns_.find(conn);
    if (it == conns_.end()) return;
    const std::string out = line + "\n";
    // 응답은 작아서 소켓 버퍼에 한 번에 들어간다. 못 쓰면 그 연결만 버린다.
    (void)!::send(it->second.fd, out.data(), out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    drop(conn);
}

void CommandServer::accept_all() {
    for (;;) {
        const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        conns_[next_id_++] = Conn{fd, {}, Clock::now()};
    }
}

void CommandServer::read_conn(int id) {
    auto it = conns_.find(id);
    if (it == conns_.end()) return;
    Conn& c = it->second;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.in.append(buf, static_cast<size_t>(n));
            if (c.in.size() > kMaxLine) return reply(id, R"({"ok":false,"error":{"code":"bad_request","message":"line too long"}})");
            continue;
        }
        if (n == 0) break;  // 상대가 닫음. 받은 줄이 있으면 처리한다
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return drop(id);
    }
    const size_t nl = c.in.find('\n');
    if (nl == std::string::npos) return;
    const std::string line = c.in.substr(0, nl);
    c.answered = true;  // 응답할 때까지 더 읽지 않는다
    c.requested = Clock::now();
    handler_(id, line);
}

void CommandServer::drop(int id) {
    const auto it = conns_.find(id);
    if (it == conns_.end()) return;
    ::close(it->second.fd);
    conns_.erase(it);
}

}  // namespace worker
