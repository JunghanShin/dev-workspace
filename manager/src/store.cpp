#include "store.hpp"

#include <chrono>

#include "log.hpp"
#include "posix_fs.hpp"

namespace pm {

namespace {
double mono_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

StoreThread::StoreThread(double timeout_s, size_t capacity)
    : timeout_s_(timeout_s), jobs_(capacity, worker::Overflow::Reject), results_(1024, worker::Overflow::Reject) {}

StoreThread::~StoreThread() { stop(0); }

void StoreThread::start() {
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
}

void StoreThread::run() {
    pthread_setname_np(pthread_self(), "pm.store");
    // 닫힌 뒤에도 남은 것은 다 처리하고 끝낸다(pop_for가 닫히고 비었을 때 nullopt)
    while (true) {
        auto job = jobs_.pop_for(std::chrono::milliseconds(200));
        if (!job) {
            if (jobs_.closed()) break;
            continue;
        }
        Result r;
        r.id = job->id;
        busy_since_.store(mono_now());
        r.ok = run_file_ops(job->ops, r.err);
        busy_since_.store(0.0);
        if (job->id != 0 && !results_.push(std::move(r))) PM_ERROR("store result queue full");
    }
    finished_.store(true);
}

void StoreThread::stop(double wait_s) {
    if (!thread_.joinable()) return;
    jobs_.close();
    // 남은 일이 끝나기를 wait_s까지 기다린다. 넘으면(디스크가 멈춤) 스레드를 떼어 둔다: 프로세스가 곧 끝난다
    const double end = mono_now() + wait_s;
    while (!finished_.load() && mono_now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (finished_.load()) thread_.join();
    else thread_.detach();
}

void StoreThread::submit(std::vector<FileOp> ops, StoreDone done) {
    Job job;
    job.id = next_id_++;
    job.ops = std::move(ops);
    const uint64_t id = job.id;
    if (!jobs_.push(std::move(job))) {
        deferred_.emplace_back(std::move(done), "store queue full");
        return;
    }
    pending_[id] = Pending{std::move(done)};
}

void StoreThread::submit_idle(FileOp op) {
    if (jobs_.stats().size > 0) return;
    Job job;
    job.id = 0;  // 결과를 받지 않음
    job.ops.push_back(std::move(op));
    jobs_.push(std::move(job));
}

void StoreThread::poll(double now) {
    std::vector<Result> got;
    results_.drain(got);
    for (auto& r : got) {
        auto it = pending_.find(r.id);
        if (it == pending_.end()) continue;  // 시간 초과로 이미 실패 처리됨
        StoreDone done = std::move(it->second.done);
        pending_.erase(it);
        if (done) done(r.ok, r.err);
    }
    // 지금 하는 요청이 상한을 넘었으면 디스크가 멈춘 것: 기다리는 요청을 모두 실패로(루프·API가 붙잡히지 않게).
    // 줄 서서 기다린 시간은 세지 않는다(fsync가 느린 보드에서 몰려온 요청이 괜히 실패하지 않게)
    const double since = busy_since_.load();
    if (since > 0 && now - since >= timeout_s_ && !pending_.empty()) {
        PM_ERROR("store write stalled over %.1fs; failing %zu request(s)", timeout_s_, pending_.size());
        auto all = std::move(pending_);
        pending_.clear();
        for (auto& [id, p] : all)
            if (p.done) p.done(false, "store timeout");
    }
    auto deferred = std::move(deferred_);
    deferred_.clear();
    for (auto& [done, err] : deferred)
        if (done) done(false, err);
}

double StoreThread::next_deadline() const {
    const double since = busy_since_.load();
    return since > 0 && !pending_.empty() ? since + timeout_s_ : 0.0;
}

}  // namespace pm
