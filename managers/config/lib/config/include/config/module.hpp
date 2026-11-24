// 모듈 config: 설정·버전 매니저(managers/config/design.md). 코어(버전·diff·남기기)를 io 스레드로 감싼다.
//
// 설정 config["config"]: keep(스냅숏 남길 수, 기본 20), root(워크스페이스 루트, 기본 $WORKSPACE_ROOT), data(데이터 폴더, 기본 $WORKSPACE_DATA)
// 스레드: 코어(호스트 루프) · config.pm(process-manager 구독: libworker) · config.io(파일·process-manager 요청을 작업으로) · config.pub
// 명령: versions·snapshots·diff(읽기), snapshot(운용 조작), restore(관리: process-manager가 개발 모드로 묶음)
// 지표: snapshots, mismatch, drift
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config/core.hpp"
#include "worker/host.hpp"
#include "worker/manager_link.hpp"
#include "worker/publisher.hpp"
#include "worker/queue.hpp"
#include "worker/thread.hpp"

namespace config {

class Module : public worker::Module {
public:
    Module() = default;
    ~Module() override { stop(); }

    std::string name() const override { return "config"; }
    void configure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    void add_pollfds(std::vector<pollfd>& fds) override;
    void on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) override;
    void on_timer(worker::TimePoint now) override;
    worker::TimePoint next_wakeup(worker::TimePoint now) const override;
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return jobs_done_; }
    bool failed(std::string* why) const override;
    void stop() override;

private:
    struct Job {
        uint64_t id = 0;  // 0: 안에서 시킨 것(답 없음)
        std::string kind; // versions | snapshot | snapshots | diff | restore | check
        Json args = Json::object();
    };
    struct Done {
        uint64_t id;
        std::string kind;
        Json result;  // ok_reply 또는 error_reply 모양
        Json state;   // 코어에 알릴 것(versions, snapshots, drift, events)
    };
    void run_io(worker::WorkerThread& t);
    Done work(const Job& j);
    Json take_snapshot();  // io 스레드
    Json list_snapshots(); // io 스레드
    bool push(Job j, worker::Reply reply);
    void drain();
    void event(const std::string& severity, const std::string& code, const std::string& text, const Json& detail);
    void publish(worker::TimePoint now);

    size_t keep_ = 20;
    std::string root_, data_;
    worker::Host* host_ = nullptr;
    std::unique_ptr<worker::ManagerSubscription> pm_;
    std::unique_ptr<worker::Publisher> pub_;
    std::unique_ptr<worker::WorkerThread> io_thread_;
    worker::BoundedQueue<Job> jobs_{8, worker::Overflow::Reject};
    worker::BoundedQueue<Done> done_{16, worker::Overflow::DropOldest};
    std::map<uint64_t, worker::Reply> replies_;
    // 코어 상태(주제)
    Json versions_ = Json::object(), mismatch_ = Json::array(), profile_, snapshots_ = Json::array();
    std::map<std::string, std::string> libworker_;
    bool drift_ = false;
    std::vector<Json> pending_events_;
    bool dirty_ = true;
    uint64_t next_job_ = 0, seq_ = 0, ev_seq_ = 0, jobs_done_ = 0;
    worker::TimePoint next_pub_{}, next_kf_{}, next_versions_{}, next_check_{};
};

}  // namespace config
