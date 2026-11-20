#include "publisher.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>

#include "log.hpp"

namespace pm {

namespace {
double mono_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr size_t kMaxOut = 4u << 20;  // 한 구독자에게 쌓일 수 있는 바이트. 넘으면 끊는다(안전 상한)
}  // namespace

Publisher::Publisher(worker::ThreadWatch& watch, double subscriber_timeout_s)
    : timeout_s_(subscriber_timeout_s),
      queue_(4096, worker::Overflow::Reject),
      dropped_(256, worker::Overflow::DropOldest),
      thread_("pm.pub", watch, [this](worker::WorkerThread& t) { run(t); }) {}

Publisher::~Publisher() { stop(); }

void Publisher::start() { thread_.start(); }

void Publisher::stop() {
    thread_.request_stop();
    queue_.close();
    thread_.join();
    for (auto& [id, s] : subs_) ::close(s.fd);
    subs_.clear();
    // 큐에 남은 Add의 fd도 닫는다
    std::vector<Msg> rest;
    queue_.drain(rest);
    for (auto& m : rest)
        if (m.kind == Msg::Kind::Add && m.fd >= 0) ::close(m.fd);
}

bool Publisher::add(int id, int fd, std::set<std::string> topics, std::string first) {
    Msg m;
    m.kind = Msg::Kind::Add;
    m.id = id;
    m.fd = fd;
    m.topics = std::move(topics);
    m.first = std::move(first);
    return queue_.push(std::move(m));
}

void Publisher::offer(Item item) {
    Msg m;
    m.kind = Msg::Kind::Offer;
    m.items.push_back(std::move(item));
    queue_.push(std::move(m));  // 차면 버림: 주기 전체 상태가 곧 다시 온다
}

void Publisher::offer_to(std::vector<int> ids, std::vector<Item> items) {
    Msg m;
    m.kind = Msg::Kind::OfferTo;
    m.ids = std::move(ids);
    m.items = std::move(items);
    queue_.push(std::move(m));
}

std::vector<int> Publisher::take_dropped() {
    std::vector<int> out;
    dropped_.drain(out);
    return out;
}

void Publisher::put(Sub& s, const Item& it) {
    if (!s.topics.count(it.topic)) return;
    auto f = s.index.find(it.key);
    if (f != s.index.end()) {
        s.pending[f->second].second = it.line;  // 최신값만(자리는 처음 들어온 곳)
    } else {
        s.index[it.key] = s.pending.size();
        s.pending.emplace_back(it.key, it.line);
    }
}

void Publisher::apply(Msg& m) {
    switch (m.kind) {
    case Msg::Kind::Add: {
        Sub s;
        s.id = m.id;
        s.fd = m.fd;
        s.topics = std::move(m.topics);
        s.out = std::move(m.first);
        subs_[m.id] = std::move(s);
        break;
    }
    case Msg::Kind::Offer:
        for (auto& [id, s] : subs_)
            for (const auto& it : m.items) put(s, it);
        break;
    case Msg::Kind::OfferTo:
        for (int id : m.ids) {
            auto f = subs_.find(id);
            if (f == subs_.end()) continue;
            for (const auto& it : m.items) put(f->second, it);
        }
        break;
    }
}

bool Publisher::flush(Sub& s, double now) {
    // 보내던 것이 다 나간 뒤에야 다음 묶음을 넣는다: 그 사이 온 것은 키마다 최신값으로 덮인다
    if (s.out.empty() && !s.pending.empty()) {
        for (auto& [k, line] : s.pending) s.out += line;
        s.pending.clear();
        s.index.clear();
    }
    while (!s.out.empty()) {
        const ssize_t n = ::send(s.fd, s.out.data(), s.out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            s.out.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return false;  // 끊김(EPIPE 등)
    }
    if (s.out.empty()) {
        s.stuck_since = 0;
        return true;
    }
    if (s.stuck_since == 0) s.stuck_since = now;
    if (s.out.size() > kMaxOut) return false;
    return now - s.stuck_since < timeout_s_;
}

void Publisher::run(worker::WorkerThread& t) {
    std::vector<pollfd> fds;
    std::vector<int> ids;
    while (!t.stopping()) {
        t.beat();
        fds.clear();
        ids.clear();
        fds.push_back({queue_.wake_fd(), POLLIN, 0});
        for (auto& [id, s] : subs_) {
            short ev = 0;
            if (!s.out.empty()) ev |= POLLOUT;
            if (!s.read_eof) ev |= POLLIN;
            fds.push_back({s.fd, ev, 0});
            ids.push_back(id);
        }
        ::poll(fds.data(), fds.size(), 200);  // 대기 상한(멈춤 감지·느린 구독자 시간 검사)
        const double now = mono_now();
        std::vector<int> drop;
        for (size_t i = 1; i < fds.size(); ++i) {
            auto it = subs_.find(ids[i - 1]);
            if (it == subs_.end()) continue;
            Sub& s = it->second;
            // 상대가 양쪽 다 닫음(HUP)·오류: 끊는다(안 그러면 poll이 계속 깨어 돈다)
            if (fds[i].revents & (POLLERR | POLLNVAL | POLLHUP)) {
                drop.push_back(s.id);
                continue;
            }
            if (fds[i].revents & POLLIN) {
                // 구독 뒤 클라이언트가 보내는 것은 읽고 버린다. 쓰기만 닫은 것(EOF)은 끊지 않는다(Python 판과 같음)
                char buf[4096];
                const ssize_t n = ::recv(s.fd, buf, sizeof buf, MSG_DONTWAIT);
                if (n == 0) s.read_eof = true;
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) drop.push_back(s.id);
            }
        }
        std::vector<Msg> msgs;
        queue_.drain(msgs, 1024);
        for (auto& m : msgs) apply(m);
        for (auto& [id, s] : subs_)
            if (!flush(s, now)) drop.push_back(id);
        for (int id : drop) {
            auto it = subs_.find(id);
            if (it == subs_.end()) continue;
            PM_INFO("subscriber dropped (%s)", it->second.out.empty() ? "closed" : "too slow");
            ::close(it->second.fd);
            subs_.erase(it);
            dropped_.push(id);
        }
    }
}

}  // namespace pm
