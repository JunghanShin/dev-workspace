// 모듈 mission: 명령 송수신 매니저(managers/mission/design.md). 코어(Dispatcher, 계약→역할)를 스레드·큐로 감싼다.
//
// 설정 config["mission"]: queue(대기 상한, 기본 4), command_timeout_ms(명령 하나, 2000), total_timeout_ms(기다림 포함, 2800),
//                        idle_close_s(쓰지 않는 명령 스레드 닫기, 60)
// 스레드: 코어(호스트 루프) · mission.pm(process-manager 구독: 워커 상태) · mission.io(계약 get_io를 10 s마다)
//         · mc.<대상>.<역할>(명령 스레드, libworker CommandClient) · mission.pub(주제 mission)
// 명령: send(운용 조작) {"target", "cmd", "args"} → 대상 워커의 응답 그대로. get_queues(읽기)
// 지표: sent, busy, timeouts, unavailable, forbidden, fast_sent, preempted, held, blocked, queues, waiting
// 우선순위·잡아 두기(priority.md), 명령 조건(conditions.md): 설정 priority·conditions
// 외부 연동 워커(external-link.md): 허용 목록(WORKSPACE_PEERS_FILE) via로 허락, 읽기·lease.renew는 빠른 줄 mf.<대상>
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mission/dispatch.hpp"
#include "worker/host.hpp"
#include "worker/manager_link.hpp"
#include "worker/peer.hpp"
#include "worker/publisher.hpp"
#include "worker/queue.hpp"
#include "worker/thread.hpp"

namespace mission {

struct Settings {
    size_t queue = 4;
    int command_timeout_ms = 2000;
    int total_timeout_ms = 2800;
    double idle_close_s = 60;
    PriorityRules priority;              // priority.md(없으면 모두 같음)
    std::vector<Condition> conditions;   // conditions.md(없으면 검사 없음)
};

class Module : public worker::Module {
public:
    Module() = default;
    ~Module() override { stop(); }

    std::string name() const override { return "mission"; }
    void configure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    void add_pollfds(std::vector<pollfd>& fds) override;
    void on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) override;
    void on_timer(worker::TimePoint now) override;
    worker::TimePoint next_wakeup(worker::TimePoint now) const override;
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return sent_ + busy_ + timeouts_; }
    bool failed(std::string* why) const override;
    void stop() override;

private:
    struct Lane {
        std::unique_ptr<worker::CommandClient> client;
        uint64_t client_id = 0;   // 보낸 명령의 CommandClient 번호
        uint64_t request_id = 0;  // 보낸 명령의 Request id
        worker::TimePoint last_used{};
    };
    struct FastLane {
        std::unique_ptr<worker::CommandClient> client;
        std::map<uint64_t, uint64_t> pending;  // CommandClient 번호 → Request id
        worker::TimePoint last_used{};
    };
    void drain();
    void send_now(const Request& r);
    void send_fast(const Request& r);
    void refresh_access();
    void flush_refused();
    void finish(uint64_t request_id, const Json& response);
    void event(const std::string& severity, const std::string& code, const std::string& text, const Json& detail);
    void publish(worker::TimePoint now, bool force);
    void run_io(worker::WorkerThread& t);

    Settings s_;
    worker::Host* host_ = nullptr;
    std::unique_ptr<Dispatcher> disp_;
    std::unique_ptr<worker::ManagerSubscription> pm_;
    std::unique_ptr<worker::Publisher> pub_;
    std::unique_ptr<worker::WorkerThread> io_thread_;
    worker::BoundedQueue<std::vector<std::string>> io_want_{1, worker::Overflow::DropOldest};  // 코어 → io: 계약을 읽을 워커들
    worker::BoundedQueue<std::map<std::string, Contract>> io_got_{1, worker::Overflow::DropOldest};  // io → 코어
    std::map<std::string, Contract> contracts_;
    std::map<std::string, std::string> states_;  // 워커 id → actual_state
    World world_;                                 // 명령 조건이 보는 것(상태·지표·모드)
    bool ids_changed_ = true;
    std::map<std::string, Lane> lanes_;               // (대상, 역할) → 명령 스레드
    std::map<std::string, FastLane> fast_;            // 대상 → 빠른 줄(읽기·lease.renew)
    std::string peers_file_;
    long long peers_mtime_ns_ = -1;
    Access access_;
    std::map<std::string, worker::TimePoint> denied_at_;  // (보낸 곳, 명령) → 마지막 이벤트(10 s에 한 번)
    std::map<uint64_t, worker::Reply> replies_;       // Request id → 보낸 곳에 답할 것
    std::vector<Json> pending_events_;
    uint64_t next_id_ = 0, seq_ = 0, ev_seq_ = 0;
    uint64_t sent_ = 0, busy_ = 0, timeouts_ = 0, unavailable_ = 0, forbidden_ = 0, fast_sent_ = 0;
    uint64_t preempted_ = 0, held_ = 0, blocked_ = 0;
    bool dirty_ = true;
    worker::TimePoint next_pub_{}, next_kf_{}, next_io_{};
};

}  // namespace mission
