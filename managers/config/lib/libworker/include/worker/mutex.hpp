// 우선순위 상속 잠금 (제공 도구, design.md 6.6.9 ③).
//
// 기본은 잠금 없이 큐로 주고받는다(6.6.1). 잠금이 꼭 필요하고, 실시간 스레드와 보통 스레드가 같은 잠금을 쓰면
// std::mutex 대신 이것을 쓴다: 보통 스레드가 잠금을 쥔 동안 실시간 스레드가 기다리면 쥔 스레드의 우선순위를
// 잠시 올려 중간 우선순위 스레드에 밀리지 않게 한다(우선순위 뒤집힘 방지). std::lock_guard·unique_lock과 같이 쓴다.
//
// 규칙: 잠금 안에서 기다리거나 입출력하지 않는다, 짧게, 순서를 고정한다, 중첩하지 않는다.
#pragma once

#include <pthread.h>

#include <system_error>

namespace worker {

class Mutex {
public:
    Mutex() {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
        const int r = pthread_mutex_init(&m_, &a);
        pthread_mutexattr_destroy(&a);
        if (r != 0) throw std::system_error(r, std::generic_category(), "pthread_mutex_init");
    }
    ~Mutex() { pthread_mutex_destroy(&m_); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() {
        if (const int r = pthread_mutex_lock(&m_); r != 0) throw std::system_error(r, std::generic_category(), "mutex lock");
    }
    bool try_lock() { return pthread_mutex_trylock(&m_) == 0; }
    void unlock() { pthread_mutex_unlock(&m_); }

private:
    pthread_mutex_t m_;
};

}  // namespace worker
