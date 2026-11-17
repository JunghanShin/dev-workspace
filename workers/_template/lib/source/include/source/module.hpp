// source 모듈: 유닛 Generator를 워커 모듈로 감싼다. ② 워커 실행 층(워커 템플릿, 워크스페이스 design.md 6.6.3·6.6.7).
//
// 이 모듈이 보여 주는 것:
//   스레드          "source.gen": 표본을 만들어 데이터 버스에 쓴다(기다리는 일은 워커 루프가 아니라 스레드에서)
//   워커 안 연결     SourcePort(port.hpp)로 같은 워커의 다른 모듈(processor)에 표본을 넘긴다
//   워커 간 연결     데이터 버스(큰 데이터, 다른 워커가 InOrder/Latest로 받음), 스트림 주제 "source"(상태), 명령 source.*
//   바로 적용 설정   rate_hz·amplitude는 다시 시작 없이(reconfigure)
//   자식 프로세스    helper_cmd가 있으면 외부 프로그램을 띄우고 감시한다(죽으면 다시 띄움)
#pragma once

#include <sys/types.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "framebus/framebus.hpp"
#include "source/generator.hpp"
#include "source/port.hpp"
#include "worker/host.hpp"
#include "worker/publisher.hpp"
#include "worker/queue.hpp"

namespace source {

struct Config {
    GeneratorConfig gen;
    int bus_slots = 8;            // 데이터 버스 슬롯 수(받는 쪽이 느릴 때 쌓일 수 있는 수)
    std::string bus_socket;       // 비우면 worker::default_socket(id, "frames")
    std::string helper_cmd;       // 비우면 자식 프로세스 없음(예: "sleep 3600")
};

class Module : public worker::Module, public SourcePort {
public:
    std::string name() const override { return "source"; }
    void configure(const worker::Json& config) override;
    bool reconfigure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    void add_pollfds(std::vector<pollfd>& fds) override;
    void on_timer(worker::TimePoint now) override;
    worker::TimePoint next_wakeup(worker::TimePoint now) const override;
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return seq_; }
    bool failed(std::string* why) const override;
    void stop() override;

    SourcePort& port() { return *this; }
    void subscribe(Listener fn) override { listeners_.push_back(std::move(fn)); }

    static Config parse(const worker::Json& c, const std::string& worker_id);

private:
    struct Made {  // source.gen 스레드 → 워커 루프
        uint64_t seq;
        int64_t ts_ms;
        std::vector<float> values;
    };
    void publish_stream(worker::TimePoint now);
    void check_helper(worker::TimePoint now);

    Config cfg_;
    std::mutex gen_mu_;  // 생성기 설정은 워커 루프(reconfigure)와 source.gen 스레드가 함께 본다
    Generator gen_;
    std::unique_ptr<framebus::Producer> bus_;
    std::unique_ptr<worker::BoundedQueue<Made>> made_;
    std::unique_ptr<worker::WorkerThread> thread_;
    std::unique_ptr<worker::Publisher> pub_;
    std::vector<Listener> listeners_;
    uint64_t seq_ = 0;
    float last_mean_ = 0;
    uint64_t stream_seq_ = 0;
    bool stream_dirty_ = false;
    worker::TimePoint next_kf_{};
    pid_t helper_ = -1;
    uint64_t helper_restarts_ = 0;
    worker::TimePoint helper_retry_{};
};

}  // namespace source
