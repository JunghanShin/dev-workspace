// 입력·출력 단계 (제공 도구, design.md 6.6.9 ③). 쓰지 않아도 된다(WorkerThread + BoundedQueue를 직접 엮어도 됨).
//
// 단계 = 스레드 하나 + 큐 하나 + 대기 상한 + 멈춤 감지 + 지표. 워커는 장치를 "한 번" 읽거나 쓰는 함수만 준다.
//
//   입력:  [스레드: read(ctx)를 계속 부름 → ctx.push(항목)] ─큐─▶ 호스트 루프가 handle(항목)  (처리 시간 예산 안에서)
//   출력:  모듈이 push(항목) ─큐─▶ [스레드: write(항목)]
//
// 모듈의 start(host) 안에서 만든다:
//     auto& rx = host.input<Msg>("fc.rx", [this](auto& in) { if (auto m = link.read(in.wait_ms())) in.push(*m); },
//                                [this](Msg& m) { core.on_rx(m); });
//     auto& tx = host.output<Frame>("fc.tx", [this](const Frame& f) { link.send(f); });
//     ... tx.push(frame);   // 막히지 않음. 거절되면 false(출력 기본 overflow = reject)
//
// 크기·넘칠 때·대기 상한·우선순위·CPU는 설정 맨 위 "stages": {"<이름>": {...}}로 바꾼다(코드 수정 없음).
// read·write 예외는 세고(<단계>_errors) wait_ms 쉰 뒤 계속한다. 끝내야 하면 worker::Fatal을 던진다.
// handle 예외는 그 항목만 버리고 센다. 단계 이름은 "<모듈>.<역할>"(스레드 이름, 15자까지).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "worker/queue.hpp"
#include "worker/thread.hpp"

namespace worker {

using Json = nlohmann::json;

// 끝내야 하는 오류(상태가 깨졌거나 되돌릴 수 없음). 호스트가 safe_state() 뒤 exit 4로 끝낸다
struct Fatal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct StageOptions {
    size_t queue = 1024;
    Overflow overflow = Overflow::DropOldest;
    int wait_ms = 100;
    ThreadOptions thread;
};

const char* overflow_name(Overflow o);
// 설정 한 칸 {"queue", "overflow", "wait_ms", "priority", "rt_priority", "nice", "cpu"} → base 위에 덮어씀.
// 잘못된 값이면 ConfigError
StageOptions parse_stage_options(const Json& j, const std::string& name, StageOptions base);

class Stage {
public:
    using Clock = std::chrono::steady_clock;
    virtual ~Stage() = default;
    const std::string& name() const { return name_; }
    virtual const char* kind() const = 0;
    virtual void stop() = 0;  // 정지 요청, 큐 닫기, join
    // 1초 지표 "k=v ..."(밀림 최대값은 읽으면 0으로)
    virtual std::string metrics() = 0;
    virtual Json status() const = 0;
    bool failed(std::string* why) const {
        if (thread_ && thread_->failed()) {
            *why = name_ + ": " + thread_->error();
            return true;
        }
        return false;
    }
    // 장애 주입(시험): 이 시간 동안 스레드가 beat 없이 쉰다(멈춤 흉내)
    void fault_pause(int ms) {
        pause_until_ns_.store(now_ns() + static_cast<int64_t>(ms) * 1000000);
    }
    // 입력 단계만
    virtual int wake_fd() const { return -1; }
    // 호스트 루프: deadline까지 처리. 남았으면 true. handle 예외 수는 단계 errors에
    virtual bool process(Clock::time_point deadline) {
        (void)deadline;
        return false;
    }

protected:
    Stage(std::string name, StageOptions opts) : name_(std::move(name)), opts_(std::move(opts)) {}
    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }
    // 주입된 멈춤이면 beat 없이 쉬고 true
    bool hold_if_paused(WorkerThread& t) {
        bool held = false;
        while (!t.stopping() && now_ns() < pause_until_ns_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            held = true;
        }
        return held;
    }
    void note_error(const char* where, const std::exception& e) {
        const uint64_t n = ++errors_;
        {
            std::lock_guard<std::mutex> lock(err_mu_);
            last_error_ = std::string(where) + ": " + e.what();
        }
        if (n <= 5 || n % 100 == 0)  // 로그가 넘치지 않게
            std::fprintf(stderr, "%s: %s error #%llu: %s\n", name_.c_str(), where, static_cast<unsigned long long>(n), e.what());
    }
    void note_lag(Clock::time_point enqueued) {
        const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - enqueued).count();
        int64_t cur = lag_max_us_.load();
        while (us > cur && !lag_max_us_.compare_exchange_weak(cur, us)) {
        }
        lag_last_us_.store(us);
    }
    std::string base_metrics(size_t size, size_t high, uint64_t dropped) {
        std::string k = name_;
        std::replace(k.begin(), k.end(), '.', '_');
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s_q=%zu %s_q_max=%zu %s_lag_ms=%.1f %s_drops=%llu %s_errors=%llu", k.c_str(), size,
                      k.c_str(), high, k.c_str(), static_cast<double>(lag_max_us_.exchange(0)) / 1000.0, k.c_str(),
                      static_cast<unsigned long long>(dropped), k.c_str(), static_cast<unsigned long long>(errors_.load()));
        return buf;
    }
    Json base_status(size_t cap, size_t size, size_t high, uint64_t dropped) const {
        Json j = {{"kind", kind()},
                  {"queue", cap},
                  {"size", size},
                  {"high_water", high},
                  {"dropped", dropped},
                  {"overflow", overflow_name(opts_.overflow)},
                  {"wait_ms", opts_.wait_ms},
                  {"lag_ms", static_cast<double>(lag_last_us_.load()) / 1000.0},
                  {"errors", errors_.load()}};
        {
            std::lock_guard<std::mutex> lock(err_mu_);
            if (!last_error_.empty()) j["last_error"] = last_error_;
        }
        if (thread_) j["thread"] = thread_->info();
        return j;
    }

    std::string name_;
    StageOptions opts_;
    std::unique_ptr<WorkerThread> thread_;
    std::atomic<uint64_t> errors_{0};
    std::atomic<int64_t> lag_max_us_{0}, lag_last_us_{0};
    std::atomic<int64_t> pause_until_ns_{0};
    mutable std::mutex err_mu_;
    std::string last_error_;
};

template <class T>
class Input : public Stage {
public:
    class Context {
    public:
        explicit Context(Input& in) : in_(in) {}
        // 큐에 넣는다(막히지 않음). 넘칠 때 동작은 단계 설정. 넣었으면(또는 DropOldest·DropNewest로 처리했으면) true
        bool push(T item) { return in_.q_.push({std::move(item), Clock::now()}); }
        // 읽기 한 번의 대기 상한(이 안에 돌아와야 함)
        int wait_ms() const { return in_.opts_.wait_ms; }
        bool stopping() const { return in_.thread_->stopping(); }

    private:
        Input& in_;
    };
    using Reader = std::function<void(Context&)>;
    using Handler = std::function<void(T&)>;

    Input(std::string name, StageOptions opts, ThreadWatch& watch, Reader read, Handler handle)
        : Stage(std::move(name), std::move(opts)), q_(opts_.queue, opts_.overflow), read_(std::move(read)),
          handle_(std::move(handle)) {
        thread_ = std::make_unique<WorkerThread>(name_, watch, [this](WorkerThread& t) {
            Context ctx(*this);
            while (!t.stopping()) {
                if (hold_if_paused(t)) continue;
                try {
                    read_(ctx);
                } catch (const Fatal&) {
                    throw;  // 스레드 실패 → 호스트가 exit 4
                } catch (const std::exception& e) {
                    note_error("read", e);
                    std::this_thread::sleep_for(std::chrono::milliseconds(opts_.wait_ms));
                }
                t.beat();
            }
        });
        thread_->set_options(opts_.thread);
        thread_->start();
    }
    const char* kind() const override { return "input"; }
    int wake_fd() const override { return q_.wake_fd(); }

    bool process(Clock::time_point deadline) override {
        while (Clock::now() < deadline) {
            auto item = q_.try_pop();
            if (!item) return false;
            note_lag(item->second);
            try {
                handle_(item->first);
            } catch (const Fatal&) {
                throw;
            } catch (const std::exception& e) {
                note_error("handle", e);
            }
        }
        return q_.stats().size > 0;
    }
    void stop() override {
        thread_->request_stop();
        q_.close();
        thread_->join();
    }
    std::string metrics() override {
        const auto s = q_.stats();
        return base_metrics(s.size, s.high_water, s.dropped);
    }
    Json status() const override {
        const auto s = q_.stats();
        return base_status(q_.capacity(), s.size, s.high_water, s.dropped);
    }

private:
    BoundedQueue<std::pair<T, Clock::time_point>> q_;
    Reader read_;
    Handler handle_;
};

template <class T>
class Output : public Stage {
public:
    using Writer = std::function<void(const T&)>;

    Output(std::string name, StageOptions opts, ThreadWatch& watch, Writer write)
        : Stage(std::move(name), std::move(opts)), q_(opts_.queue, opts_.overflow), write_(std::move(write)) {
        thread_ = std::make_unique<WorkerThread>(name_, watch, [this](WorkerThread& t) {
            while (!t.stopping()) {
                if (hold_if_paused(t)) continue;
                if (auto item = q_.pop_for(std::chrono::milliseconds(opts_.wait_ms))) {
                    note_lag(item->second);
                    try {
                        write_(item->first);
                    } catch (const Fatal&) {
                        throw;
                    } catch (const std::exception& e) {
                        note_error("write", e);
                    }
                }
                t.beat();
            }
        });
        thread_->set_options(opts_.thread);
        thread_->start();
    }
    const char* kind() const override { return "output"; }
    // 막히지 않는다. reject(출력 기본)면 가득 찼을 때 false
    bool push(T item) { return q_.push({std::move(item), Clock::now()}); }
    void stop() override {
        thread_->request_stop();
        q_.close();
        thread_->join();
    }
    std::string metrics() override {
        const auto s = q_.stats();
        return base_metrics(s.size, s.high_water, s.dropped);
    }
    Json status() const override {
        const auto s = q_.stats();
        return base_status(q_.capacity(), s.size, s.high_water, s.dropped);
    }

private:
    BoundedQueue<std::pair<T, Clock::time_point>> q_;
    Writer write_;
};

}  // namespace worker
