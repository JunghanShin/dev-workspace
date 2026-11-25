#include "hub.hpp"

#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>

#include "log.hpp"
#include "sys.hpp"

namespace gw {

// ---- Waker ----

void Waker::poke() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++gen_;
    }
    cv_.notify_all();
}

unsigned long long Waker::gen() {
    std::lock_guard<std::mutex> lock(mu_);
    return gen_;
}

void Waker::wait(unsigned long long seen, double timeout_s) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::duration<double>(timeout_s), [&] { return gen_ != seen; });
}

// ---- Subscriber ----

void Subscriber::offer(const Json& msg, bool keyframe) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        merge_.offer(msg, keyframe);
    }
    cv_.notify_one();
    if (waker_) waker_->poke();
}

std::optional<Json> Subscriber::take(double timeout_s) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!merge_.dirty() && !closed_)
        cv_.wait_for(lock, std::chrono::duration<double>(timeout_s), [&] { return merge_.dirty() || closed_; });
    if (!merge_.dirty()) return std::nullopt;
    return merge_.take();
}

void Subscriber::close() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
    }
    cv_.notify_one();
}

// ---- Upstream ----

Upstream::Upstream(std::string worker_id, Hub& hub, SourceOpener open, StreamTiming timing)
    : id_(std::move(worker_id)), hub_(hub), open_(std::move(open)), timing_(timing) {
    beat_.store(mono_now());
}

Upstream::~Upstream() {
    stop();
    join();
}

void Upstream::start() {
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void Upstream::stop() {
    stop_.store(true);
    std::lock_guard<std::mutex> lock(wait_mu_);
    wait_cv_.notify_all();
}

void Upstream::join() {
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

bool Upstream::up() const {
    std::lock_guard<std::mutex> lock(mu_);
    return up_;
}

size_t Upstream::subscribers() const {
    std::lock_guard<std::mutex> lock(mu_);
    return subs_.size();
}

double Upstream::beat_age(double now) const { return now - beat_.load(); }

void Upstream::beat() { beat_.store(mono_now()); }

bool Upstream::subscribe(const std::shared_ptr<Subscriber>& sub) {
    std::lock_guard<std::mutex> lock(mu_);
    if (dead_) return false;
    subs_.insert(sub);
    idle_since_ = -1;
    // 기준 상태를 잠금 안에서 넣는다: 그 사이 온 변경분이 기준 상태보다 먼저 들어가 덮이는 일이 없게
    sub->offer(state_.snapshot(up_), true);
    return true;
}

void Upstream::unsubscribe(const std::shared_ptr<Subscriber>& sub) {
    std::lock_guard<std::mutex> lock(mu_);
    subs_.erase(sub);
    if (subs_.empty()) idle_since_ = mono_now();
}

bool Upstream::idle_locked(double now) const {
    return subs_.empty() && idle_since_ >= 0 && now - idle_since_ > timing_.idle_close_s;
}

bool Upstream::retire() {
    // 화면이 모두 떠난 지 오래면 정리하기로 정한다(subscribe와 같은 잠금 안에서)
    std::lock_guard<std::mutex> lock(mu_);
    if (idle_locked(mono_now())) dead_ = true;
    return dead_;
}

void Upstream::broadcast(const Json& msg, bool keyframe, std::vector<std::shared_ptr<Subscriber>> subs) {
    for (auto& s : subs) s->offer(msg, keyframe);
}

void Upstream::set_up(bool up) {
    std::vector<std::shared_ptr<Subscriber>> subs;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (up_ == up) return;
        up_ = up;
        if (up) return;
        subs.assign(subs_.begin(), subs_.end());
    }
    broadcast(Json{{"stream", "down"}}, false, std::move(subs));
}

void Upstream::sleep_retry() {
    std::unique_lock<std::mutex> lock(wait_mu_);
    wait_cv_.wait_for(lock, std::chrono::duration<double>(timing_.retry_s), [&] { return stop_.load(); });
}

void Upstream::pump(int fd) {
    bool fresh = true;
    std::string buf;
    while (!stop_.load()) {
        beat();
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (idle_locked(mono_now())) return;
        }
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, 1000);  // 1 s마다 정지·화면 없음을 본다
        if (r == 0) continue;
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        char tmp[65536];
        const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n == 0) return;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            GW_INFO("stream %s: %s", id_.c_str(), errno_text(errno).c_str());
            return;
        }
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.size() > timing_.max_line) {
            GW_INFO("stream %s: line too long", id_.c_str());
            return;
        }
        size_t start = 0, nl;
        while ((nl = buf.find('\n', start)) != std::string::npos) {
            auto msg = parse_json(buf.substr(start, nl - start));
            start = nl + 1;
            if (!msg || !msg->is_object()) continue;
            // 붙은 직후에는 기준 상태부터 받는다. 그 전 것은 버린다
            auto kf = msg->find("kf");
            if (fresh && (kf == msg->end() || !py_truthy(*kf))) continue;
            Json out;
            std::vector<std::shared_ptr<Subscriber>> subs;
            {
                std::lock_guard<std::mutex> lock(mu_);
                out = state_.apply(*msg, fresh);
                if (fresh) up_ = true;
                subs.assign(subs_.begin(), subs_.end());
            }
            broadcast(out, fresh, std::move(subs));
            fresh = false;
        }
        buf.erase(0, start);
    }
}

void Upstream::run() {
    const std::string name = ("gw.up." + id_).substr(0, 15);  // 리눅스 스레드 이름은 15자까지
    pthread_setname_np(pthread_self(), name.c_str());
    while (!stop_.load()) {
        beat();
        if (retire()) break;
        std::string why;
        const int fd = open_(id_, why);
        beat();
        if (fd < 0) {
            GW_DEBUG("stream %s: %s", id_.c_str(), why.c_str());
            set_up(false);
            sleep_retry();
            continue;
        }
        GW_INFO("stream %s: connected %s", id_.c_str(), why.c_str());
        pump(fd);
        ::close(fd);
        set_up(false);
        if (!stop_.load()) sleep_retry();
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        dead_ = true;
    }
    hub_.gone(this);
    running_.store(false);  // 마지막: 이 뒤로는 멤버를 건드리지 않는다(reap이 join)
}

// ---- Hub ----

Hub::Hub(SourceOpener open, StreamTiming timing) : open_(std::move(open)), timing_(timing) {}

Hub::~Hub() { stop_all(); }

std::pair<std::shared_ptr<Upstream>, std::shared_ptr<Subscriber>> Hub::subscribe(const std::string& worker_id, double hz,
                                                                                 std::shared_ptr<Waker> waker) {
    reap();
    auto sub = std::make_shared<Subscriber>(hz, std::move(waker));
    std::shared_ptr<Upstream> up;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = ups_.find(worker_id);
    if (it != ups_.end() && it->second->subscribe(sub)) {
        up = it->second;
    } else {
        if (it != ups_.end()) done_.push_back(it->second);  // 정리 중인 것: 스레드가 끝나면 reap
        up = std::make_shared<Upstream>(worker_id, *this, open_, timing_);
        ups_[worker_id] = up;
        up->subscribe(sub);
        up->start();
    }
    return {up, sub};
}

void Hub::gone(Upstream* up) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = ups_.find(up->id());
    if (it != ups_.end() && it->second.get() == up) {
        done_.push_back(it->second);
        ups_.erase(it);
    }
}

void Hub::reap() {
    std::vector<std::shared_ptr<Upstream>> finished;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto keep = std::stable_partition(done_.begin(), done_.end(), [](const auto& u) { return u->running(); });
        finished.assign(std::make_move_iterator(keep), std::make_move_iterator(done_.end()));
        done_.erase(keep, done_.end());
    }
    for (auto& u : finished) u->join();
}

void Hub::stop_all() {
    std::vector<std::shared_ptr<Upstream>> all;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& kv : ups_) all.push_back(kv.second);
        for (auto& u : done_) all.push_back(u);
        ups_.clear();
        done_.clear();
    }
    for (auto& u : all) u->stop();
    for (auto& u : all) u->join();
}

std::map<std::string, size_t> Hub::stats() {
    std::lock_guard<std::mutex> lock(mu_);
    std::map<std::string, size_t> out;
    for (auto& kv : ups_) out[kv.first] = kv.second->subscribers();
    return out;
}

std::string Hub::stalled(double limit_s) {
    const double now = mono_now();
    std::string out;
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& kv : ups_)
        if (kv.second->running() && kv.second->beat_age(now) > limit_s) out += (out.empty() ? "gw.up." : ",gw.up.") + kv.first;
    return out;
}

// ---- SseSession ----

SseSession::SseSession(std::shared_ptr<Upstream> up, std::shared_ptr<Subscriber> sub, double ping_s)
    : up_(std::move(up)), sub_(std::move(sub)), ping_s_(ping_s), last_write_(mono_now()) {}

SseSession::~SseSession() { finish(); }

void SseSession::finish() {
    if (finished_.exchange(true)) return;
    up_->unsubscribe(sub_);
    sub_->close();
}

bool SseSession::step(const std::function<bool(const std::string&)>& write, const std::atomic<bool>& stopping) {
    if (!retry_sent_) {
        retry_sent_ = true;
        last_write_ = mono_now();
        return write("retry: 2000\n\n");
    }
    // 화면의 hz: 마지막 전송 뒤 간격이 지날 때까지(정지 요청은 0.2 s 안에 봄)
    while (!stopping.load()) {
        const double wait = last_ + sub_->min_interval() - mono_now();
        if (wait <= 0) break;
        std::this_thread::sleep_for(std::chrono::duration<double>(std::min(wait, 0.2)));
    }
    if (stopping.load()) return false;
    auto msg = sub_->take(1.0);
    const double now = mono_now();
    if (!msg) {
        if (now - last_write_ >= ping_s_) {
            if (!write(": ping\n\n")) return false;
            last_write_ = now;
        }
        return true;
    }
    if (!write("data: " + compact(*msg) + "\n\n")) return false;
    last_ = last_write_ = now;
    return true;
}

// ---- MuxSession ----

MuxSession::MuxSession(std::vector<Feed> feeds, std::shared_ptr<Waker> waker, double ping_s)
    : waker_(std::move(waker)), ping_s_(ping_s), last_write_(mono_now()) {
    for (auto& f : feeds) es_.push_back(Entry{std::move(f)});
}

MuxSession::~MuxSession() { finish(); }

void MuxSession::finish() {
    if (finished_.exchange(true)) return;
    for (auto& e : es_) {
        e.f.up->unsubscribe(e.f.sub);
        e.f.sub->close();
    }
}

bool MuxSession::step(const std::function<bool(const std::string&)>& write, const std::atomic<bool>& stopping) {
    if (!retry_sent_) {
        retry_sent_ = true;
        last_write_ = mono_now();
        return write("retry: 2000\n\n");
    }
    if (stopping.load()) return false;
    const unsigned long long seen = waker_->gen();  // 훑기 전에: 훑는 중 온 offer도 아래 wait가 바로 깸
    const double now = mono_now();
    double wake = now + 0.2;  // 정지 요청을 0.2 s 안에 봄
    std::string out;
    for (auto& e : es_) {
        const double at = e.last + e.f.sub->min_interval();
        if (now < at) {  // 이 워커의 hz 간격이 아직
            wake = std::min(wake, at);
            continue;
        }
        auto msg = e.f.sub->take(0);
        if (!msg) continue;
        const std::string line = mux_line(e.f.id, std::move(*msg), e.down);
        e.last = now;
        if (!line.empty()) out += "data: " + line + "\n\n";
    }
    if (!out.empty()) {
        if (!write(out)) return false;
        last_write_ = now;
        return true;
    }
    if (now - last_write_ >= ping_s_) {
        if (!write(": ping\n\n")) return false;
        last_write_ = now;
        return true;
    }
    waker_->wait(seen, std::max(0.0, wake - now));
    return true;
}

}  // namespace gw
