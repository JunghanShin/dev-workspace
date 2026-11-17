#include "worker/publisher.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

namespace worker {

namespace {

constexpr auto kPollTimeout = std::chrono::milliseconds(100);

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

Publisher::Publisher(ThreadWatch& watch, Options opt)
    : opt_(opt),
      queue_(opt.queue, Overflow::DropOldest),
      thread_(opt.thread_name, watch, [this](WorkerThread& t) { run(t); }) {}

Publisher::~Publisher() { close(); }

bool Publisher::open(const std::string& path, std::string* error) {
    sockaddr_un addr{};
    if (path.size() >= sizeof(addr.sun_path)) {
        *error = "stream socket path too long: " + path;
        return false;
    }
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && !make_dirs(path.substr(0, slash))) {
        *error = "cannot create folder for " + path + ": " + std::strerror(errno);
        return false;
    }
    ::unlink(path.c_str());
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
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
    path_ = path;
    thread_.start();
    return true;
}

void Publisher::close() {
    if (listen_fd_ < 0) return;
    queue_.close();
    thread_.request_stop();
    thread_.join();
    ::close(listen_fd_);
    listen_fd_ = -1;
    ::unlink(path_.c_str());
}

void Publisher::publish(std::string line) {
    if (subscribers_.load() == 0) return;
    line.push_back('\n');
    queue_.push(std::move(line));
}

void Publisher::run(WorkerThread& t) {
    std::vector<Sub> subs;
    std::vector<std::string> batch;
    std::vector<pollfd> fds;

    auto drop = [&](size_t i) {
        ::close(subs[i].fd);
        subs.erase(subs.begin() + static_cast<long>(i));
        subscribers_.store(static_cast<int>(subs.size()));
    };
    // 보낼 수 있는 만큼 보낸다. 끊겼으면 false
    auto flush = [&](Sub& s) {
        while (!s.out.empty()) {
            const ssize_t n = ::send(s.fd, s.out.data(), s.out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0) {
                s.out.erase(0, static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            return false;
        }
        return true;
    };

    while (!t.stopping()) {
        t.beat();
        fds.clear();
        fds.push_back({listen_fd_, POLLIN, 0});
        fds.push_back({queue_.wake_fd(), POLLIN, 0});
        for (const auto& s : subs) {
            // 구독자는 보내지 않는다. POLLIN은 끊김(읽으면 0) 감지용
            fds.push_back({s.fd, static_cast<short>(POLLIN | (s.out.empty() ? 0 : POLLOUT)), 0});
        }
        const int rc = ::poll(fds.data(), fds.size(), static_cast<int>(kPollTimeout.count()));
        if (rc < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
        }

        // 새 구독자
        if (fds[0].revents & POLLIN) {
            for (;;) {
                const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) break;
                if (static_cast<int>(subs.size()) >= opt_.max_subscribers) {
                    ::close(fd);
                    continue;
                }
                subs.push_back(Sub{fd, {}});
                subscribers_.store(static_cast<int>(subs.size()));
                keyframe_.store(true);
            }
        }

        // 구독자 상태(끊김·쓰기 가능). fds 순서는 [listen, queue, subs...]
        for (size_t i = subs.size(); i-- > 0;) {
            const short ev = fds.size() > i + 2 ? fds[i + 2].revents : 0;
            bool alive = true;
            if (ev & (POLLERR | POLLHUP | POLLNVAL)) alive = false;
            if (alive && (ev & POLLIN)) {
                char buf[256];
                const ssize_t n = ::recv(subs[i].fd, buf, sizeof buf, MSG_DONTWAIT);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) alive = false;
            }
            if (alive && (ev & POLLOUT)) alive = flush(subs[i]);
            if (!alive) drop(i);
        }

        // 발행할 줄 → 구독자 버퍼
        batch.clear();
        queue_.drain(batch);
        if (batch.empty() || subs.empty()) continue;
        for (size_t i = subs.size(); i-- > 0;) {
            Sub& s = subs[i];
            for (const auto& line : batch) s.out += line;
            bool alive = flush(s);
            if (alive && s.out.size() > opt_.max_backlog) {
                alive = false;  // 따라오지 못하는 구독자
                dropped_subs_.fetch_add(1);
            }
            if (!alive) drop(i);
        }
    }
    for (const auto& s : subs) ::close(s.fd);
    subscribers_.store(0);
}

}  // namespace worker
