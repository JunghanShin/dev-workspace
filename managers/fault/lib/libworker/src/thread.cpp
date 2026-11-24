#include "worker/thread.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
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

std::atomic<int64_t>* ThreadWatch::beat_cell(int slot) {
    std::lock_guard<std::mutex> lock(mu_);
    return &slots_.at(static_cast<size_t>(slot))->last_ns;
}

void ThreadWatch::beat(int slot) { beat_cell(slot)->store(to_ns(Clock::now()), std::memory_order_relaxed); }

void ThreadWatch::retire(int slot) {
    std::lock_guard<std::mutex> lock(mu_);
    slots_.at(static_cast<size_t>(slot))->retired.store(true);
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

// ---- 스레드 우선순위·CPU ---------------------------------------------------------------------------

nlohmann::json ThreadOptions::to_json() const {
    nlohmann::json j = {{"priority", priority}};
    if (priority == "realtime") j["rt_priority"] = rt_priority;
    else j["nice"] = nice;
    if (!cpus.empty()) j["cpu"] = cpus;
    return j;
}

nlohmann::json apply_thread_options(const ThreadOptions& opts) {
    nlohmann::json out = {{"requested", opts.to_json()}, {"applied", true}};
    std::string errors;
    auto fail = [&](const std::string& what, int err) {
        errors += (errors.empty() ? "" : "; ") + what + ": " + std::strerror(err);
    };
    if (opts.priority == "realtime") {
        sched_param sp{};
        sp.sched_priority = opts.rt_priority;
        if (const int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); r != 0)
            fail("realtime " + std::to_string(opts.rt_priority) + " (LimitRTPRIO?)", r);
    } else if (opts.nice != 0) {
        // Linux: nice는 스레드(태스크)마다
        const auto tid = static_cast<id_t>(::syscall(SYS_gettid));
        if (::setpriority(PRIO_PROCESS, tid, opts.nice) != 0) fail("nice " + std::to_string(opts.nice), errno);
    }
    if (!opts.cpus.empty()) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c : opts.cpus)
            if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &set);
        if (const int r = pthread_setaffinity_np(pthread_self(), sizeof set, &set); r != 0) fail("cpu affinity", r);
    }
    if (!errors.empty()) {
        out["applied"] = false;
        out["error"] = errors;
    }
    return out;
}

// ---- WorkerThread --------------------------------------------------------------------------------

WorkerThread::WorkerThread(std::string name, ThreadWatch& watch, Body body)
    : name_(std::move(name)), watch_(watch), body_(std::move(body)), slot_(watch.add(name_)),
      beat_(watch.beat_cell(slot_)) {}

WorkerThread::~WorkerThread() {
    request_stop();
    join();
}

void WorkerThread::beat() { beat_->store(to_ns(ThreadWatch::Clock::now()), std::memory_order_relaxed); }

void WorkerThread::start() {
    if (thread_.joinable()) return;
    running_.store(true);
    beat();
    thread_ = std::thread([this] { run(); });
}

void WorkerThread::join() {
    if (thread_.joinable()) thread_.join();
}

std::string WorkerThread::error() const {
    std::lock_guard<std::mutex> lock(err_mu_);
    return error_;
}

nlohmann::json WorkerThread::info() const {
    std::lock_guard<std::mutex> lock(err_mu_);
    nlohmann::json j = {{"name", name_}, {"requested", opts_.to_json()}, {"applied", true}};
    if (applied_.is_object()) {
        j["applied"] = applied_.value("applied", true);
        if (applied_.contains("error")) j["error"] = applied_["error"];
    }
    return j;
}

void WorkerThread::run() {
    // 운영체제 스레드 이름(top -H, 시스템 모니터 스레드 표). 15자까지. 규칙: "<모듈>.<역할>"
    pthread_setname_np(pthread_self(), name_.substr(0, 15).c_str());
    if (!opts_.is_default()) {
        nlohmann::json r = apply_thread_options(opts_);
        if (!r.value("applied", true))
            std::fprintf(stderr, "%s: thread options not applied (running normal): %s\n", name_.c_str(),
                         r.value("error", "").c_str());
        std::lock_guard<std::mutex> lock(err_mu_);
        applied_ = std::move(r);
    }
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
    // 정지 요청으로 끝났거나, 실패했거나, 본문이 할 일을 다 하고 끝났으면 멈춤으로 보지 않는다.
    // 실패는 failed()로 Core가 판단한다
    watch_.retire(slot_);
    running_.store(false);
}

}  // namespace worker
