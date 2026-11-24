// 발행(텔레메트리 스트림): 워커가 바뀐 값을 구독자에게 밀어 준다 (design.md 6.6.2, 9.7).
//
// 명령은 요청-응답(명령 소켓), 텔레메트리는 발행-구독(이 스트림 소켓)으로 나눈다.
//
//   Core ──publish(line)──▶ pub 큐(DropOldest) ──▶ PUB 스레드 ──▶ 구독자들(Unix SOCK_STREAM, JSON 한 줄씩)
//
// - publish는 막히지 않는다. 큐가 차면 오래된 것부터 버린다(텔레메트리는 최신이 중요)
// - 구독자마다 보낼 버퍼가 있고, 상한(max_backlog)을 넘으면 그 구독자를 끊는다(느린 구독자가 다른 구독자·Core를 막지 못함)
// - 새 구독자가 붙으면 keyframe_requested()가 참이 된다. Core는 전체 상태(기준 상태)를 한 번 발행한다
// - PUB 스레드는 WorkerThread로 돌고 루프마다 beat한다(ThreadWatch 멈춤 감지)
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "worker/queue.hpp"
#include "worker/thread.hpp"

namespace worker {

class Publisher {
public:
    struct Options {
        size_t queue = 1024;                // pub 큐(줄 수)
        size_t max_backlog = 512 * 1024;    // 구독자 하나에 쌓일 수 있는 바이트. 넘으면 끊는다
        int max_subscribers = 16;
        std::string thread_name = "pub";    // PUB 스레드 이름(모듈 안에서는 "<모듈>.pub")
    };

    Publisher(ThreadWatch& watch, Options opt);
    Publisher(ThreadWatch& watch) : Publisher(watch, Options{}) {}
    ~Publisher();
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    // 기존 파일을 지우고 bind(권한 0600, 폴더가 없으면 만든다), PUB 스레드 시작
    bool open(const std::string& path, std::string* error);
    void close();

    // JSON 한 줄(줄바꿈 없이). 막히지 않는다. 구독자가 없으면 버린다
    void publish(std::string line);
    bool has_subscribers() const { return subscribers_.load() > 0; }
    // 새 구독자가 붙은 뒤 한 번 참(읽으면 지워진다)
    bool keyframe_requested() { return keyframe_.exchange(false); }

    const std::string& path() const { return path_; }
    WorkerThread& thread() { return thread_; }
    int subscribers() const { return subscribers_.load(); }
    uint64_t dropped_subscribers() const { return dropped_subs_.load(); }
    BoundedQueue<std::string>::Stats queue_stats() const { return queue_.stats(); }

private:
    struct Sub {
        int fd;
        std::string out;
    };
    void run(WorkerThread& t);

    Options opt_;
    BoundedQueue<std::string> queue_;
    WorkerThread thread_;
    std::string path_;
    int listen_fd_ = -1;
    std::atomic<int> subscribers_{0};
    std::atomic<bool> keyframe_{false};
    std::atomic<uint64_t> dropped_subs_{0};
};

}  // namespace worker
