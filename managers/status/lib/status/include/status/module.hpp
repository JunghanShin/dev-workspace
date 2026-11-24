// 모듈 status: 시스템 상태 매니저(managers/status/design.md). 코어(Health)를 구독·발행으로 감싼다.
//
// 설정 config["status"]: required(필수 워커 글롭, 비면 autostart 워커), stale_s(기본 3), metrics(지표 조건),
//                       publish_ms(주제 발행 간격, 기본 1000. 바뀌면 바로)
// 스레드: status.pm(process-manager 구독, libworker ManagerSubscription) → 큐 → 코어(호스트 루프)
//         ss.<id>(워커·매니저 스트림 구독, 연결 여부만 봄) → 큐 → 코어
//         발행: libworker Publisher(status.pub), 주제 system(io/system.json)
// 구독할 곳: 입력 파일 many.streams(libworker ManyInputs, 1 s마다 확인)
// 명령: get_system(읽기). 지표: health(0/1/2), workers, faults, pm_link, subs, subs_up
#pragma once

#include <map>
#include <memory>
#include <string>

#include "status/health.hpp"
#include "worker/host.hpp"
#include "worker/manager_link.hpp"
#include "worker/peer.hpp"
#include "worker/publisher.hpp"

namespace status {

class Module : public worker::Module {
public:
    Module() = default;
    ~Module() override { stop(); }

    std::string name() const override { return "status"; }
    void configure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    void add_pollfds(std::vector<pollfd>& fds) override;
    void on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) override;
    void on_timer(worker::TimePoint now) override;
    worker::TimePoint next_wakeup(worker::TimePoint now) const override;
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return published_; }
    bool failed(std::string* why) const override;
    void stop() override;

private:
    void drain(worker::TimePoint now);
    void sync_subs();
    void publish(worker::TimePoint now, bool force);
    Json system_now();

    Rules rules_;
    int publish_ms_ = 1000;
    std::unique_ptr<Health> health_;
    worker::Host* host_ = nullptr;
    std::unique_ptr<worker::ManagerSubscription> pm_;
    std::unique_ptr<worker::ManyInputs> many_;
    std::map<std::string, std::unique_ptr<worker::Subscriber>> subs_;
    std::unique_ptr<worker::Publisher> pub_;
    std::string boot_id_;
    Json current_ = Json::object();
    Json signature_;  // 건강·연결만 모은 것(바뀌면 바로 발행)
    static constexpr size_t kKeepEvents = 64;  // 구독자가 없을 때 쌓아 둘 최근 이벤트 수
    std::vector<Json> pending_events_;
    bool dirty_ = true;
    worker::TimePoint next_pub_{}, next_kf_{}, next_sync_{};
    uint64_t seq_ = 0, published_ = 0;
};

}  // namespace status
