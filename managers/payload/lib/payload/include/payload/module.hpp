// 모듈 payload: 모듈 운용 매니저(managers/payload/design.md). 코어(Modules)를 구독·쓰기 요청으로 감싼다.
//
// 설정 config["payload"]: groups {"<이름>": ["<워커 id 글롭>", ...]}
// 스레드: 코어(호스트 루프) · payload.pm(process-manager 구독) · payload.io(계약 get_io: 역할)
//         · payload.sys(status-manager 주제 system, 입력 "system") · payload.ctl(process-manager start·stop 요청) · payload.pub
// 명령: list(읽기), power {"module", "on"}, power_group {"group", "on"}(운용 조작)
// 지표: modules, running, power_requests, power_failed
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "payload/modules.hpp"
#include "worker/host.hpp"
#include "worker/manager_link.hpp"
#include "worker/peer.hpp"
#include "worker/publisher.hpp"
#include "worker/queue.hpp"
#include "worker/thread.hpp"

namespace payload {

class Module : public worker::Module {
public:
    Module() = default;
    ~Module() override { stop(); }

    std::string name() const override { return "payload"; }
    void configure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    void add_pollfds(std::vector<pollfd>& fds) override;
    void on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) override;
    void on_timer(worker::TimePoint now) override;
    worker::TimePoint next_wakeup(worker::TimePoint now) const override;
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return power_requests_; }
    bool failed(std::string* why) const override;
    void stop() override;

private:
    struct CtlReq {
        uint64_t job;       // 묶음 요청이면 그 번호
        std::string id, op; // 워커, start|stop
    };
    struct CtlRes {
        uint64_t job;
        std::string id, op;
        Json response;
    };
    struct Job {
        worker::Reply reply;
        size_t remaining = 0;
        Json results = Json::object();
        bool group = false;
        std::string origin;
    };
    void drain();
    void publish(worker::TimePoint now);
    bool submit(uint64_t job, const std::string& id, bool on);
    void event(const std::string& severity, const std::string& code, const std::string& text, const Json& detail);
    void run_io(worker::WorkerThread& t);
    void run_ctl(worker::WorkerThread& t);

    Groups groups_;
    worker::Host* host_ = nullptr;
    std::unique_ptr<Modules> mods_;
    std::unique_ptr<worker::ManagerSubscription> pm_;
    std::unique_ptr<worker::Subscriber> sys_;
    std::unique_ptr<worker::Publisher> pub_;
    std::unique_ptr<worker::WorkerThread> io_thread_, ctl_thread_;
    worker::BoundedQueue<std::vector<std::string>> io_want_{1, worker::Overflow::DropOldest};
    worker::BoundedQueue<std::map<std::string, std::vector<std::string>>> io_got_{1, worker::Overflow::DropOldest};
    worker::BoundedQueue<CtlReq> ctl_q_{16, worker::Overflow::Reject};
    worker::BoundedQueue<CtlRes> ctl_done_{64, worker::Overflow::DropOldest};
    std::map<uint64_t, Job> jobs_;
    std::vector<Json> pending_events_;
    bool ids_changed_ = true, dirty_ = true;
    uint64_t next_job_ = 0, seq_ = 0, ev_seq_ = 0, power_requests_ = 0, power_failed_ = 0;
    worker::TimePoint next_pub_{}, next_kf_{}, next_io_{};
};

}  // namespace payload
