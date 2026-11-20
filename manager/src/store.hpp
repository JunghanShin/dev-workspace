// pm.store: 파일 쓰기 스레드 (docs/process-manager.md 2.1).
//
// 제어 루프는 fsync를 기다리지 않는다. 루프가 submit → 이 스레드가 임시 → fsync → rename →
// 결과를 결과 큐로 → 루프가 wake_fd로 깨어 poll()에서 done을 부른다(그때 API 응답).
//
// - 요청 큐 64, 차면 그 요청만 실패(done(false))
// - 요청 하나 상한 2 s: 하던 요청이 넘으면(디스크 멈춤) 기다리는 요청을 모두 실패로 본다(늦게 온 결과는 버림).
//   줄 서서 기다린 시간은 세지 않는다(fsync가 느린 보드)
// - 이 스레드는 ThreadWatch에 넣지 않는다: fsync가 오래 걸리는 것은 멈춤이 아니라 "그 요청 실패"로 다룬다
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <thread>
#include <utility>
#include <vector>

#include "ports.hpp"
#include "worker/queue.hpp"

namespace pm {

class StoreThread : public Store {
public:
    explicit StoreThread(double timeout_s = 2.0, size_t capacity = 64);
    ~StoreThread() override;

    void start();
    // 남은 요청을 wait_s까지 처리하고 끝낸다(끝낼 때 상태 파일을 잃지 않게)
    void stop(double wait_s);

    // ---- 루프 스레드에서만 ----
    void submit(std::vector<FileOp> ops, StoreDone done) override;
    // 낮은 우선순위 일(로그 회전): 큐가 비어 있을 때만 넣는다
    void submit_idle(FileOp op);
    int wake_fd() const { return results_.wake_fd(); }
    void poll(double now);              // 결과·시간 초과·못 넣은 요청의 done을 부른다
    double next_deadline() const;       // 없으면 0
    size_t in_flight() const { return pending_.size(); }
    bool has_deferred() const { return !deferred_.empty(); }

private:
    struct Job {
        uint64_t id = 0;
        std::vector<FileOp> ops;
    };
    struct Result {
        uint64_t id = 0;
        bool ok = false;
        std::string err;
    };
    struct Pending {
        StoreDone done;
    };
    void run();

    const double timeout_s_;
    worker::BoundedQueue<Job> jobs_;
    worker::BoundedQueue<Result> results_;
    std::thread thread_;
    uint64_t next_id_ = 1;                                       // 루프만
    std::map<uint64_t, Pending> pending_;                        // 루프만
    std::vector<std::pair<StoreDone, std::string>> deferred_;    // 루프만
    std::atomic<bool> finished_{false};
    std::atomic<double> busy_since_{0.0};  // 지금 하는 요청을 시작한 시각(0 = 쉼)
};

}  // namespace pm
