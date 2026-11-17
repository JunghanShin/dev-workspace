// 워커 안의 스레드와 멈춤 감지 (워커 구조 기준, design.md 6.6.1).
//
// 구조: 막힐 수 있는 경계(수신, 송신, 무거운 처리)마다 WorkerThread 하나. 스레드 사이는 BoundedQueue로만 잇는다.
// 상태는 Core(주 스레드) 하나만 가진다.
//
// 멈춤 감지: 각 스레드는 루프를 돌 때마다 beat()한다. Core는 ThreadWatch::all_alive()가 참일 때만
// 커널 워치독(arm_watchdog)과 Manager heartbeat를 갱신한다. 한 스레드라도 stall_after보다 오래 beat가 없으면
// Core가 갱신을 멈추고 → 커널 워치독이 워커를 끝내고 → Manager가 다시 띄운다(워커는 스스로 판단해 종료).
// 그래서 스레드 본문은 stall_after보다 오래 막히면 안 된다(수신·대기는 시간 제한을 둔다).
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

namespace worker {

class ThreadWatch {
public:
    using Clock = std::chrono::steady_clock;

    explicit ThreadWatch(std::chrono::milliseconds stall_after) : stall_after_(stall_after) {}
    ThreadWatch(const ThreadWatch&) = delete;
    ThreadWatch& operator=(const ThreadWatch&) = delete;

    // 감시할 스레드를 등록하고 번호를 받는다. 스레드를 시작하기 전에(Core에서) 부른다
    int add(const std::string& name);
    // 살아 있음. 어느 스레드에서 불러도 된다(잠금 없음)
    void beat(int slot);
    // 멈춘 것으로 보지 않을 스레드(정상 종료한 스레드). 그 뒤로는 검사하지 않는다
    void retire(int slot);

    // 모든 스레드가 stall_after 안에 beat했는지. 멈춘 스레드가 있으면 stalled에 이름을 넣는다
    bool all_alive(Clock::time_point now, std::string* stalled = nullptr) const;
    // 지표용: {"<이름>": 마지막 beat 이후 ms, ...}
    nlohmann::json ages_ms(Clock::time_point now) const;

    std::chrono::milliseconds stall_after() const { return stall_after_; }

private:
    struct Slot {
        std::string name;
        std::atomic<int64_t> last_ns{0};
        std::atomic<bool> retired{false};
    };
    const std::chrono::milliseconds stall_after_;
    mutable std::mutex mu_;  // slots_ 목록 자체(등록)만 보호. beat는 원자 변수만 쓴다
    std::vector<std::unique_ptr<Slot>> slots_;
};

// 이름 있는 스레드. 본문은 stopping()이 참이 될 때까지 돌며 매번 beat()한다.
//
//     WorkerThread rx("rx", watch, [&](WorkerThread& t) {
//         while (!t.stopping()) {
//             link.read_some(100ms);   // 시간 제한 있는 대기
//             t.beat();
//         }
//     });
//     rx.start();  ...  rx.request_stop(); rx.join();
//
// 본문에서 예외가 나면 잡아서 failed()로 알리고 스레드를 끝낸다(멈춤이 아니라 실패로 보고, Core가 종료를 판단).
class WorkerThread {
public:
    using Body = std::function<void(WorkerThread&)>;

    WorkerThread(std::string name, ThreadWatch& watch, Body body);
    ~WorkerThread();  // 아직 돌고 있으면 정지 요청 후 join
    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    void start();
    void request_stop() { stop_.store(true); }
    bool stopping() const { return stop_.load(); }
    void beat() { watch_.beat(slot_); }
    void join();

    const std::string& name() const { return name_; }
    bool running() const { return running_.load(); }
    // 본문이 예외로 끝났으면 true, 그 메시지는 error()
    bool failed() const { return failed_.load(); }
    std::string error() const;

private:
    void run();

    std::string name_;
    ThreadWatch& watch_;
    Body body_;
    int slot_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    mutable std::mutex err_mu_;
    std::string error_;
};

}  // namespace worker
