// 스레드 사이 큐 (워커 구조 기준: 스레드는 큐로만 잇는다. design.md 6.6.1).
//
// - 용량 제한. 넘칠 때 정책을 정한다: DropOldest(텔레메트리처럼 최신이 중요) / Reject(명령처럼 잃으면 안 됨, 넣은 쪽이 실패를 앎)
//   / DropNewest(넣은 쪽이 몰라도 되는 새것을 버림)
// - push는 절대 막히지 않는다(생산자는 기다리지 않는다)
// - 소비자는 pop_for(시간 제한)로 기다리거나, poll()에 wake_fd()를 넣어 다른 fd와 함께 기다린다
// - close() 후에는 push가 실패하고, 남은 것을 다 꺼내면 pop이 비어 있음을 돌려준다(정지용)
// - 통계: 현재 크기, 최대 크기, 넣은·꺼낸·버린 수(지표로 보고)
#pragma once

#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace worker {

enum class Overflow { DropOldest, Reject, DropNewest };

template <class T>
class BoundedQueue {
public:
    struct Stats {
        size_t size = 0;
        size_t high_water = 0;  // 지금까지 가장 많이 쌓인 수
        uint64_t pushed = 0;
        uint64_t popped = 0;
        uint64_t dropped = 0;   // 넘쳐서 버리거나 거절한 수
    };

    BoundedQueue(size_t capacity, Overflow policy) : capacity_(capacity), policy_(policy) {
        if (capacity_ == 0) throw std::invalid_argument("queue capacity must be > 0");
        efd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    }
    ~BoundedQueue() {
        if (efd_ >= 0) ::close(efd_);
    }
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // 막히지 않는다. DropOldest: 가득 차면 가장 오래된 것을 버리고 넣는다(true).
    // Reject: 가득 차면 넣지 않고 false. DropNewest: 가득 차면 넣지 않고 true(버린 수만 셈). close() 뒤에는 false.
    bool push(T item) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (closed_) return false;
            if (items_.size() >= capacity_) {
                ++stats_.dropped;
                if (policy_ == Overflow::Reject) return false;
                if (policy_ == Overflow::DropNewest) return true;
                items_.pop_front();
            }
            items_.push_back(std::move(item));
            ++stats_.pushed;
            if (items_.size() > stats_.high_water) stats_.high_water = items_.size();
        }
        cv_.notify_one();
        signal();
        return true;
    }

    // 하나 꺼낸다. 비어 있으면 timeout까지 기다린다. 시간이 지났거나 닫히고 비었으면 nullopt.
    std::optional<T> pop_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mu_);
        if (!cv_.wait_for(lock, timeout, [this] { return !items_.empty() || closed_; })) return std::nullopt;
        return take_locked();
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mu_);
        return take_locked();
    }

    // 최대 max개를 한 번에 꺼내 out 뒤에 붙인다. 꺼낸 수를 돌려준다(Core가 poll 깨어난 뒤 한꺼번에 처리)
    size_t drain(std::vector<T>& out, size_t max = SIZE_MAX) {
        std::lock_guard<std::mutex> lock(mu_);
        size_t n = 0;
        while (!items_.empty() && n < max) {
            out.push_back(std::move(items_.front()));
            items_.pop_front();
            ++n;
        }
        stats_.popped += n;
        if (items_.empty()) clear_signal_locked();  // 남은 것이 있으면 계속 깨어 있게
        return n;
    }

    // 정지: 기다리는 소비자를 깨운다. 이후 push는 실패한다
    void close() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            closed_ = true;
        }
        cv_.notify_all();
        signal();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mu_);
        return closed_;
    }

    // 비어 있지 않거나 닫혔을 때 읽기 가능해지는 fd. poll(POLLIN)에 넣는다. 꺼낼 때 자동으로 비워진다
    int wake_fd() const { return efd_; }

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mu_);
        Stats s = stats_;
        s.size = items_.size();
        return s;
    }

    size_t capacity() const { return capacity_; }

private:
    std::optional<T> take_locked() {
        if (items_.empty()) {
            clear_signal_locked();
            return std::nullopt;
        }
        T item = std::move(items_.front());
        items_.pop_front();
        ++stats_.popped;
        if (items_.empty()) clear_signal_locked();
        return item;
    }
    void signal() {
        if (efd_ < 0) return;
        const uint64_t one = 1;
        [[maybe_unused]] ssize_t n = ::write(efd_, &one, sizeof one);
    }
    void clear_signal_locked() {
        if (efd_ < 0 || closed_) return;  // 닫혔으면 계속 깨어 있게 둔다
        uint64_t v;
        [[maybe_unused]] ssize_t n = ::read(efd_, &v, sizeof v);
    }

    const size_t capacity_;
    const Overflow policy_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<T> items_;
    Stats stats_;
    bool closed_ = false;
    int efd_ = -1;
};

}  // namespace worker
