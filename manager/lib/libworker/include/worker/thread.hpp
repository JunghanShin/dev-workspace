// 워커 안의 스레드와 멈춤 감지 (워커 구조 기준, design.md 6.6.1).
//
// 구조: 막힐 수 있는 경계(수신, 송신, 무거운 처리)마다 WorkerThread 하나. 스레드 사이는 BoundedQueue로만 잇는다.
// 상태는 Core(주 스레드) 하나만 가진다. 구조는 워커 개발자가 정한다(design.md 6.6.9).
//
// 멈춤 감지(워커 계약): 각 스레드는 루프를 돌 때마다 beat()한다. Core는 ThreadWatch::all_alive()가 참일 때만
// 커널 워치독(arm_watchdog)과 Manager heartbeat를 갱신한다. 한 스레드라도 stall_after보다 오래 beat가 없으면
// 호스트가 safe_state() 뒤 스스로 끝내고(exit 4) → Manager가 다시 띄운다.
// 그래서 스레드 본문은 stall_after보다 오래 막히면 안 된다(수신·대기는 시간 제한을 둔다).
//
// 스레드 우선순위·CPU(제공 도구, design.md 6.6.9 ③): WorkerThread::set_options(ThreadOptions) 또는 스레드 안에서
// apply_thread_options(). 권한이 없으면 경고만 하고 보통으로 계속 돈다(죽지 않음). 결과는 info()에.
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

    // 감시할 스레드를 등록하고 번호를 받는다. 언제 불러도 된다(다른 스레드가 beat하는 중에도)
    int add(const std::string& name);
    // 살아 있음. 어느 스레드에서 불러도 된다
    void beat(int slot);
    // beat가 쓰는 시각 칸(등록한 동안 주소가 바뀌지 않음). WorkerThread가 잠금 없이 beat하려고 쓴다
    std::atomic<int64_t>* beat_cell(int slot);
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
    mutable std::mutex mu_;  // slots_ 목록. Slot 자체는 unique_ptr라 등록 뒤 주소가 바뀌지 않는다
    std::vector<std::unique_ptr<Slot>> slots_;
};

// 스레드 우선순위·CPU (제공 도구). 기본값은 아무것도 바꾸지 않음
struct ThreadOptions {
    std::string priority = "normal";  // "normal" | "realtime"(SCHED_FIFO rt_priority)
    int rt_priority = 50;             // 1~99
    int nice = 0;                     // normal일 때 -20~19
    std::vector<int> cpus;            // 비어 있으면 전체

    bool is_default() const { return priority == "normal" && nice == 0 && cpus.empty(); }
    nlohmann::json to_json() const;
};

// 지금(부른) 스레드에 옵션을 적용한다. 돌려준 값: {"requested", "applied": bool, "error"?: "..."}.
// 실패해도(권한 없음 등) 예외를 던지지 않는다. 그 스레드는 보통 우선순위로 계속 돈다
nlohmann::json apply_thread_options(const ThreadOptions& opts);

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

    // start 전에: 우선순위·CPU(스레드가 시작할 때 스스로 적용)
    void set_options(const ThreadOptions& opts) { opts_ = opts; }
    void start();
    void request_stop() { stop_.store(true); }
    bool stopping() const { return stop_.load(); }
    void beat();
    void join();

    const std::string& name() const { return name_; }
    bool running() const { return running_.load(); }
    // 본문이 예외로 끝났으면 true, 그 메시지는 error()
    bool failed() const { return failed_.load(); }
    std::string error() const;
    // {"name", "requested", "applied", "error"?} (시스템 모니터 워커 보기)
    nlohmann::json info() const;

private:
    void run();

    std::string name_;
    ThreadWatch& watch_;
    Body body_;
    int slot_;
    std::atomic<int64_t>* beat_;
    ThreadOptions opts_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    mutable std::mutex err_mu_;
    std::string error_;
    nlohmann::json applied_;  // err_mu_로 보호
};

}  // namespace worker
