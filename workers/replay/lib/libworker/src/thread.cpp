#include "worker/thread.hpp"

#include <pthread.h>

#include <exception>

namespace worker {

namespace {
int64_t to_ns(ThreadWatch::Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}
}  // namespace

int ThreadWatch::add(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    auto slot = std::make_unique<Slot>();
    slot->name = name;
    slot->last_ns.store(to_ns(Clock::now()));
    slots_.push_back(std::move(slot));
    return static_cast<int>(slots_.size()) - 1;
}

void ThreadWatch::beat(int slot) {
    // 등록이 끝난 뒤(스레드 시작 전)에는 slots_가 바뀌지 않으므로 잠금 없이 읽는다
    slots_[static_cast<size_t>(slot)]->last_ns.store(to_ns(Clock::now()), std::memory_order_relaxed);
}

void ThreadWatch::retire(int slot) {
    slots_[static_cast<size_t>(slot)]->retired.store(true);
}

bool ThreadWatch::all_alive(Clock::time_point now, std::string* stalled) const {
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t limit = std::chrono::duration_cast<std::chrono::nanoseconds>(stall_after_).count();
    const int64_t now_ns = to_ns(now);
    bool ok = true;
    for (const auto& s : slots_) {
        if (s->retired.load()) continue;
        if (now_ns - s->last_ns.load(std::memory_order_relaxed) > limit) {
            if (stalled) *stalled += (stalled->empty() ? "" : ",") + s->name;
            ok = false;
        }
    }
    return ok;
}

nlohmann::json ThreadWatch::ages_ms(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    nlohmann::json out = nlohmann::json::object();
    const int64_t now_ns = to_ns(now);
    for (const auto& s : slots_) {
        if (s->retired.load()) continue;
        out[s->name] = (now_ns - s->last_ns.load(std::memory_order_relaxed)) / 1000000;
    }
    return out;
}

WorkerThread::WorkerThread(std::string name, ThreadWatch& watch, Body body)
    : name_(std::move(name)), watch_(watch), body_(std::move(body)), slot_(watch.add(name_)) {}

WorkerThread::~WorkerThread() {
    request_stop();
    join();
}

void WorkerThread::start() {
    if (thread_.joinable()) return;
    running_.store(true);
    watch_.beat(slot_);
    thread_ = std::thread([this] { run(); });
}

void WorkerThread::join() {
    if (thread_.joinable()) thread_.join();
}

std::string WorkerThread::error() const {
    std::lock_guard<std::mutex> lock(err_mu_);
    return error_;
}

void WorkerThread::run() {
    // 운영체제 스레드 이름(top -H, 시스템 모니터 스레드 표). 15자까지. 규칙: "<모듈>.<역할>"
    pthread_setname_np(pthread_self(), name_.substr(0, 15).c_str());
    try {
        body_(*this);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(err_mu_);
        error_ = e.what();
        failed_.store(true);
    } catch (...) {
        std::lock_guard<std::mutex> lock(err_mu_);
        error_ = "unknown exception";
        failed_.store(true);
    }
    // 정지 요청으로 끝났으면 멈춤으로 보지 않는다. 실패는 failed()로 Core가 판단한다
    if (stop_.load() || failed_.load()) watch_.retire(slot_);
    running_.store(false);
}

}  // namespace worker
