// 워커 간 통신 — 받는 쪽·부르는 쪽 도구 (design.md 6.6.7).
//
// 워커끼리 주고받는 통로는 셋이고, 상대는 워커 id(설정 칸, 스키마 형식 "worker_id")로 정한다:
//   요청·응답   상대의 명령 소켓      CommandClient: 워커 루프를 막지 않게 별도 스레드에서 보내고 응답은 큐로
//   신호·상태   상대의 스트림 소켓    Subscriber: 상대 발행(Publisher)을 구독, 끊기면 다시 붙고 받은 줄은 큐로
//   큰 데이터   상대의 프레임 버스    framebus::Consumer(Latest 또는 InOrder). 이 파일에는 없음(libframebus)
//
// 공통 규칙: 상대가 없거나 다시 시작 중이어도 살아서 기다리며 다시 붙는다(connected()로 보고 → 지표 <모듈>_link).
// 상대를 시작·정지하지 않는다(통신만). 두 도구 모두 이름 있는 WorkerThread(ThreadWatch 감시) + 크기 제한 큐이고,
// 워커 루프는 wake_fd()를 add_pollfds에 넣고 next()로 꺼낸다.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "nlohmann/json.hpp"
#include "worker/queue.hpp"
#include "worker/thread.hpp"

namespace worker {

// ---- 신호·상태 구독 ------------------------------------------------------------------------
class Subscriber {
public:
    struct Options {
        std::string thread_name = "sub";  // 모듈 안에서는 "<모듈>.sub"(15자 이하)
        double retry_sec = 1.0;           // 끊긴 뒤 다시 붙는 간격
        size_t queue = 256;               // 받은 줄 큐(넘치면 오래된 것부터 버림: 최신이 중요)
        std::string socket;               // 비우면 상대 워커 id의 기본 스트림 소켓(default_stream_socket)
    };
    // 받은 한 줄(design.md 9.7). down이면 연결이 끊긴 것(다시 붙으면 kf: true인 기준 상태부터 온다)
    struct Update {
        bool down = false;
        bool kf = false;
        nlohmann::json topics = nlohmann::json::object();
        nlohmann::json events = nlohmann::json::array();
    };

    Subscriber(ThreadWatch& watch, std::string peer_id, Options opt);
    Subscriber(ThreadWatch& watch, std::string peer_id) : Subscriber(watch, std::move(peer_id), Options{}) {}
    ~Subscriber();
    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;

    void start();
    void stop();
    int wake_fd() const { return queue_.wake_fd(); }
    std::optional<Update> next() { return queue_.try_pop(); }

    bool connected() const { return connected_.load(); }
    uint64_t received() const { return received_.load(); }      // 받은 줄 수
    uint64_t reconnects() const { return reconnects_.load(); }  // 다시 붙은 횟수
    const std::string& peer() const { return peer_; }
    const std::string& path() const { return path_; }
    WorkerThread& thread() { return thread_; }

private:
    void run(WorkerThread& t);
    std::string peer_, path_;
    Options opt_;
    BoundedQueue<Update> queue_;
    WorkerThread thread_;
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> received_{0}, reconnects_{0};
};

// ---- 요청·응답(비동기) ---------------------------------------------------------------------
class CommandClient {
public:
    struct Options {
        std::string thread_name = "cmd";  // 모듈 안에서는 "<모듈>.cmd"
        int timeout_ms = 1500;            // 명령 하나의 응답 대기 상한(thread_stall_sec보다 짧게)(워커 루프가 아니라 이 스레드가 기다린다)
        size_t queue = 64;                // 보낼 명령 큐(넘치면 send가 0: 잃으면 안 되므로 거절)
        std::string socket;               // 비우면 상대 워커 id의 기본 명령 소켓(default_command_socket)
        // 명령 입구로 감싸기(libworker 0.16, 워크스페이스 managers/mission/external-link.md): 허용 목록(peers.json)에
        // "entry"가 있고 상대가 입구가 아니면 {"cmd": "mission.send", "args": {"target", "cmd", "args"}}로 입구에 보낸다.
        // 응답은 대상 워커의 것이 그대로 온다. 입구 자신(mission)·socket을 직접 준 경우는 끔
        bool via_entry = true;
    };
    struct Reply {
        uint64_t id = 0;               // send가 돌려준 번호
        std::string cmd;
        nlohmann::json response;       // {"ok": true, "data": ...} 또는 {"ok": false, "error": {...}}(연결 실패 포함)
    };

    CommandClient(ThreadWatch& watch, std::string peer_id, Options opt);
    CommandClient(ThreadWatch& watch, std::string peer_id) : CommandClient(watch, std::move(peer_id), Options{}) {}
    ~CommandClient();
    CommandClient(const CommandClient&) = delete;
    CommandClient& operator=(const CommandClient&) = delete;

    void start();
    void stop();
    // 명령을 큐에 넣는다. 번호(1부터)를 돌려준다. 큐가 차면 0
    uint64_t send(const std::string& cmd, const nlohmann::json& args = nlohmann::json::object());
    // 봉투에 칸을 더해 보낸다(예: {"origin": "screen"}, 명령 입구 매니저가 넘길 때. libworker 0.14)
    uint64_t send(const std::string& cmd, const nlohmann::json& args, const nlohmann::json& envelope);
    int wake_fd() const { return replies_.wake_fd(); }
    std::optional<Reply> next() { return replies_.try_pop(); }

    bool reachable() const { return reachable_.load(); }  // 마지막 명령이 상대에게 닿았는지
    const std::string& peer() const { return peer_; }
    uint64_t sent_via_entry() const { return via_count_.load(); }  // 입구로 감싸 보낸 수
    WorkerThread& thread() { return thread_; }

private:
    struct Request {
        uint64_t id;
        std::string cmd;
        nlohmann::json args;
        nlohmann::json envelope;  // 봉투에 더할 칸(없으면 null)
    };
    void run(WorkerThread& t);
    std::string entry();  // peers.json의 entry.id(바뀐 시각을 보고 다시 읽음, 명령 스레드만 부름). 없으면 빈 글
    std::string peer_, path_;
    Options opt_;
    std::string peers_file_, entry_;
    long long peers_mtime_ns_ = -1;
    std::atomic<uint64_t> via_count_{0};
    BoundedQueue<Request> requests_;
    BoundedQueue<Reply> replies_;
    WorkerThread thread_;
    std::atomic<uint64_t> next_id_{1};
    std::atomic<bool> reachable_{false};
};

}  // namespace worker
