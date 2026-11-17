#include "framebus/framebus.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace framebus {

namespace {

constexpr uint32_t kMagic = 0x53554246;  // "FBUS"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kFrame = 1;    // 생산자 → 소비자
constexpr uint32_t kRelease = 2;  // 소비자 → 생산자

struct Hello {
    uint32_t magic;
    uint32_t version;
    uint32_t slots;
    uint32_t slot_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    char pixel[16];
};

struct Msg {
    uint32_t type;
    int32_t slot;
    uint64_t frame_id;
    int64_t ts_mono_ns;
    uint32_t bytes;
    uint32_t pad;
};

std::string err(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

bool make_addr(const std::string& path, sockaddr_un& addr, std::string* error) {
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        if (error) *error = "socket path too long: " + path;
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

}  // namespace

// ---- 생산자 ---------------------------------------------------------------------------

Producer::Producer(std::string path, Format format, int slots)
    : path_(std::move(path)), format_(std::move(format)), slots_(slots) {}

Producer::~Producer() { stop(); }

bool Producer::start(std::string* error) {
    if (slots_ < 1 || format_.frame_bytes() == 0) {
        if (error) *error = "framebus: slots and frame size must be positive";
        return false;
    }
    const size_t page = 4096;
    slot_bytes_ = (format_.frame_bytes() + page - 1) / page * page;
    mem_fd_ = memfd_create("framebus", MFD_CLOEXEC);
    if (mem_fd_ < 0 || ftruncate(mem_fd_, static_cast<off_t>(slot_bytes_ * slots_)) != 0) {
        if (error) *error = err("framebus memfd");
        stop();
        return false;
    }
    void* p = mmap(nullptr, slot_bytes_ * slots_, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd_, 0);
    if (p == MAP_FAILED) {
        if (error) *error = err("framebus mmap");
        stop();
        return false;
    }
    base_ = static_cast<uint8_t*>(p);

    sockaddr_un addr;
    if (!make_addr(path_, addr, error)) {
        stop();
        return false;
    }
    listen_fd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    ::unlink(path_.c_str());
    if (listen_fd_ < 0 || bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(listen_fd_, 8) != 0) {
        if (error) *error = err(("framebus socket " + path_).c_str());
        stop();
        return false;
    }
    if (pipe2(wake_, O_CLOEXEC | O_NONBLOCK) != 0) {
        if (error) *error = err("framebus pipe");
        stop();
        return false;
    }
    refs_.assign(slots_, 0);
    writing_.assign(slots_, false);
    running_ = true;
    thread_ = std::thread([this] { serve(); });
    return true;
}

void Producer::stop() {
    if (running_) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            running_ = false;
        }
        char c = 1;
        (void)!write(wake_[1], &c, 1);
    }
    if (thread_.joinable()) thread_.join();
    for (auto& c : consumers_) ::close(c.fd);
    consumers_.clear();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        ::unlink(path_.c_str());
        listen_fd_ = -1;
    }
    for (int& fd : wake_) {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    if (base_) munmap(base_, slot_bytes_ * slots_);
    base_ = nullptr;
    if (mem_fd_ >= 0) ::close(mem_fd_);
    mem_fd_ = -1;
}

std::optional<Producer::Slot> Producer::acquire() {
    std::lock_guard<std::mutex> lock(mu_);
    for (int i = 0; i < slots_; ++i) {
        if (refs_[i] == 0 && !writing_[i]) {
            writing_[i] = true;
            return Slot{i, base_ + slot_bytes_ * i, format_.frame_bytes()};
        }
    }
    ++stats_.dropped;
    return std::nullopt;
}

void Producer::abandon(const Slot& slot) {
    std::lock_guard<std::mutex> lock(mu_);
    if (slot.index >= 0 && slot.index < slots_) writing_[slot.index] = false;
}

void Producer::publish(const Slot& slot, uint64_t frame_id, int64_t ts_mono_ns) {
    publish(slot, frame_id, ts_mono_ns, slot.bytes);
}

void Producer::publish(const Slot& slot, uint64_t frame_id, int64_t ts_mono_ns, size_t bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    if (slot.index < 0 || slot.index >= slots_) return;
    writing_[slot.index] = false;
    Msg m{kFrame, slot.index, frame_id, ts_mono_ns, static_cast<uint32_t>(std::min(bytes, slot.bytes)), 0};
    for (size_t i = 0; i < consumers_.size();) {
        const ssize_t n = send(consumers_[i].fd, &m, sizeof m, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(sizeof m)) {
            ++refs_[slot.index];
            ++consumers_[i].held[slot.index];
            ++i;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ++stats_.skipped;  // 그 소비자의 소켓이 찼다. 이 프레임만 건너뛴다
            ++i;
        } else {
            drop_consumer(i);  // 끊긴 소비자
        }
    }
    ++stats_.published;
}

Stats Producer::stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    Stats s = stats_;
    s.consumers = static_cast<int>(consumers_.size());
    return s;
}

// mu_를 잡은 채로 부른다
void Producer::drop_consumer(size_t i) {
    for (int s = 0; s < slots_; ++s) refs_[s] -= consumers_[i].held[s];
    ::close(consumers_[i].fd);
    consumers_.erase(consumers_.begin() + static_cast<long>(i));
}

void Producer::accept_one() {
    const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    Hello h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.slots = static_cast<uint32_t>(slots_);
    h.slot_bytes = static_cast<uint32_t>(slot_bytes_);
    h.width = format_.width;
    h.height = format_.height;
    h.stride = format_.stride;
    std::strncpy(h.pixel, format_.pixel.c_str(), sizeof h.pixel - 1);

    iovec iov{&h, sizeof h};
    char ctrl[CMSG_SPACE(sizeof(int))] = {};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof ctrl;
    cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(c), &mem_fd_, sizeof(int));
    if (sendmsg(fd, &msg, MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof h)) {
        ::close(fd);
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    consumers_.push_back(Consumer{fd, std::vector<int>(slots_, 0)});
}

void Producer::serve() {
    std::vector<pollfd> fds;
    for (;;) {
        fds.clear();
        fds.push_back({wake_[0], POLLIN, 0});
        fds.push_back({listen_fd_, POLLIN, 0});
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!running_) return;
            for (const auto& c : consumers_) fds.push_back({c.fd, POLLIN, 0});
        }
        if (poll(fds.data(), fds.size(), 200) <= 0) continue;
        if (fds[0].revents) {
            char buf[16];
            while (read(wake_[0], buf, sizeof buf) > 0) {
            }
        }
        if (fds[1].revents & POLLIN) accept_one();
        for (size_t k = 2; k < fds.size(); ++k) {
            if (!fds[k].revents) continue;
            std::lock_guard<std::mutex> lock(mu_);
            auto it = std::find_if(consumers_.begin(), consumers_.end(),
                                   [&](const Consumer& c) { return c.fd == fds[k].fd; });
            if (it == consumers_.end()) continue;
            const size_t i = static_cast<size_t>(it - consumers_.begin());
            bool gone = (fds[k].revents & (POLLHUP | POLLERR)) != 0;
            for (;;) {
                Msg m;
                const ssize_t n = recv(it->fd, &m, sizeof m, MSG_DONTWAIT);
                if (n == 0) {
                    gone = true;
                    break;
                }
                if (n < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) gone = true;
                    break;
                }
                if (n == static_cast<ssize_t>(sizeof m) && m.type == kRelease && m.slot >= 0 && m.slot < slots_ &&
                    it->held[m.slot] > 0) {
                    --it->held[m.slot];
                    --refs_[m.slot];
                }
            }
            if (gone) drop_consumer(i);
        }
    }
}

// ---- 소비자 ---------------------------------------------------------------------------

Consumer::Consumer(std::string path, Mode mode) : path_(std::move(path)), mode_(mode) {}

Consumer::~Consumer() { close(); }

void Consumer::close() {
    pending_.clear();  // 연결이 끊기면 생산자가 이 소비자의 참조를 모두 푼다
    if (base_) munmap(base_, map_bytes_);
    base_ = nullptr;
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool Consumer::connect(std::string* error) {
    close();
    sockaddr_un addr;
    if (!make_addr(path_, addr, error)) return false;
    fd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd_ < 0 || ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (error) *error = err(("framebus connect " + path_).c_str());
        close();
        return false;
    }
    timeval tv{2, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    Hello h{};
    iovec iov{&h, sizeof h};
    char ctrl[CMSG_SPACE(sizeof(int))] = {};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof ctrl;
    const ssize_t n = recvmsg(fd_, &msg, MSG_CMSG_CLOEXEC);
    int mem_fd = -1;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) std::memcpy(&mem_fd, CMSG_DATA(c), sizeof(int));
    }
    if (n != static_cast<ssize_t>(sizeof h) || h.magic != kMagic || h.version != kVersion || mem_fd < 0 ||
        h.slots == 0 || h.slot_bytes == 0) {
        if (error) *error = "framebus: bad hello from " + path_;
        if (mem_fd >= 0) ::close(mem_fd);
        close();
        return false;
    }
    slots_ = static_cast<int>(h.slots);
    slot_bytes_ = h.slot_bytes;
    map_bytes_ = slot_bytes_ * slots_;
    void* p = mmap(nullptr, map_bytes_, PROT_READ, MAP_SHARED, mem_fd, 0);
    ::close(mem_fd);
    if (p == MAP_FAILED) {
        if (error) *error = err("framebus mmap");
        close();
        return false;
    }
    base_ = static_cast<uint8_t*>(p);
    format_.width = h.width;
    format_.height = h.height;
    format_.stride = h.stride;
    h.pixel[sizeof h.pixel - 1] = 0;
    format_.pixel = h.pixel;
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    return true;
}

bool Consumer::send_release(int slot) {
    if (fd_ < 0) return false;
    Msg m{kRelease, slot, 0, 0, 0, 0};
    return send(fd_, &m, sizeof m, MSG_DONTWAIT | MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof m);
}

void Consumer::release(const Frame& f) { send_release(f.slot); }

bool Consumer::fill(Frame* out) {
    if (pending_.empty()) return false;
    const Note n = pending_.front();
    pending_.pop_front();
    out->slot = n.slot;
    out->data = base_ + slot_bytes_ * n.slot;
    out->bytes = std::min<size_t>(n.bytes, slot_bytes_);
    out->frame_id = n.frame_id;
    out->ts_mono_ns = n.ts_mono_ns;
    return true;
}

Consumer::Result Consumer::next(int timeout_ms, Frame* out) {
    if (fd_ < 0) return Result::Disconnected;
    if (mode_ == Mode::InOrder && fill(out)) return Result::Frame;  // 이미 받아 둔 것부터
    pollfd p{fd_, POLLIN, 0};
    const int r = poll(&p, 1, timeout_ms);
    if (r == 0) return Result::Timeout;
    if (r < 0) return errno == EINTR ? Result::Timeout : Result::Disconnected;
    for (;;) {
        Msg m;
        const ssize_t n = recv(fd_, &m, sizeof m, MSG_DONTWAIT);
        if (n == 0) {
            close();
            return Result::Disconnected;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close();
            return Result::Disconnected;
        }
        if (n != static_cast<ssize_t>(sizeof m) || m.type != kFrame || m.slot < 0 || m.slot >= slots_) continue;
        if (mode_ == Mode::Latest && !pending_.empty()) {
            send_release(pending_.front().slot);  // 최신 우선: 앞의 것은 바로 반납
            pending_.clear();
            ++skipped_;
        }
        pending_.push_back(Note{m.slot, m.frame_id, m.ts_mono_ns, m.bytes});
    }
    if (fill(out)) return Result::Frame;
    return (p.revents & (POLLHUP | POLLERR)) ? (close(), Result::Disconnected) : Result::Timeout;
}

}  // namespace framebus
