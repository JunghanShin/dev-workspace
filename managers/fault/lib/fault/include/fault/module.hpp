// 모듈 fault: 이상 판단 매니저(managers/fault/design.md). 코어(Engine)를 구독·대응 요청으로 감싼다.
//
// 설정 config["fault"]: rules(규칙 목록, design.md 2장). 비우면 아무것도 판단하지 않음
// 스레드: 코어(호스트 루프, 200 ms마다 판정) · fault.sys(status 주제 system, 입력 "system") · fs.<id>(이벤트, 입력 "streams" many)
//         · fault.pm(process-manager 구독: 지표·운용 모드) · fault.cmd(mission.send, 입력 "mission") · fault.pub
// 명령: list(읽기), ack {"id"}(운용 조작). 지표: active, worst, raised, actions, action_failed
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fault/engine.hpp"
#include "worker/host.hpp"
#include "worker/manager_link.hpp"
#include "worker/peer.hpp"
#include "worker/publisher.hpp"

namespace fault {

class Module : public worker::Module {
public:
    Module() = default;
    ~Module() override { stop(); }

    std::string name() const override { return "fault"; }
    void configure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    void add_pollfds(std::vector<pollfd>& fds) override;
    void on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) override;
    void on_timer(worker::TimePoint now) override;
    worker::TimePoint next_wakeup(worker::TimePoint now) const override;
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return ticks_; }
    bool failed(std::string* why) const override;
    void stop() override;

private:
    void drain();
    void sync_subs();
    void evaluate();
    void event(const Json& e);  // 엔진 이벤트 → 공통 형식으로 쌓기
    void publish(worker::TimePoint now);

    std::vector<Rule> rules_;
    worker::Host* host_ = nullptr;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<worker::ManagerSubscription> pm_;
    std::unique_ptr<worker::ManyInputs> many_;
    std::map<std::string, std::unique_ptr<worker::Subscriber>> subs_;
    std::unique_ptr<worker::Subscriber> sys_;
    std::unique_ptr<worker::CommandClient> mission_;
    std::unique_ptr<worker::Publisher> pub_;
    std::map<uint64_t, std::pair<std::string, size_t>> sent_;  // CommandClient 번호 → (이상, 대응 칸)
    std::vector<Json> pending_events_;
    std::string access_mode_ = "operation";
    bool dirty_ = true;
    uint64_t seq_ = 0, ev_seq_ = 0, ticks_ = 0, raised_ = 0, escalated_ = 0, actions_ = 0, action_failed_ = 0;
    worker::TimePoint next_pub_{}, next_kf_{}, next_sync_{}, next_eval_{};
};

}  // namespace fault
