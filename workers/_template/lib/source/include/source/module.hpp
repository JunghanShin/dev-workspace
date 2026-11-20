// source 모듈: 유닛 Generator를 워커 모듈로 감싼다. ② 워커 실행 층(워커 템플릿, 워크스페이스 design.md 6.6.3·6.6.7).
//
// 이 모듈이 보여 주는 것:
//   입력 단계       "source.gen"(제공 도구 host.input, design.md 6.6.9 ③): 스레드가 표본을 만들어 데이터 버스에 쓰고
//                   큐로 워커 루프에 넘긴다. 스레드·큐·대기 상한·멈춤 감지·지표는 템플릿이 하고, 모듈은 "한 번 만들기"와
//                   "하나 처리하기"만 쓴다. 크기·우선순위는 설정 "stages"로
//   안전 상태       safe_state(): 끝나기 전에 장치를 안전하게(예제는 기록만)
//   잠금            생성기 설정을 워커 루프와 스레드가 함께 봐서 우선순위 상속 잠금 worker::Mutex(짧게, 안에서 기다리지 않음)
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
#include "worker/mutex.hpp"
#include "worker/publisher.hpp"

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
    void on_timer(worker::TimePoint now) override;
    worker::TimePoint next_wakeup(worker::TimePoint now) const override;
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return seq_; }
    bool failed(std::string* why) const override;
    void safe_state() override;
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
    void make_once(worker::Input<Made>::Context& in);  // source.gen 스레드: 때가 되면 한 번 만들기
    void take(Made& m);                                  // 워커 루프: 하나 처리하기
    void publish_stream(worker::TimePoint now);
    void check_helper(worker::TimePoint now);

    Config cfg_;
    mutable worker::Mutex gen_mu_;  // 생성기 설정은 워커 루프(reconfigure)와 source.gen 스레드가 함께 본다
    Generator gen_;
    std::unique_ptr<framebus::Producer> bus_;
    std::unique_ptr<worker::Publisher> pub_;
    std::vector<Listener> listeners_;
    uint64_t seq_ = 0;
    float last_mean_ = 0;
    int64_t last_ts_ns_ = 0;  // 마지막 값 배열을 만든 시각(CLOCK_MONOTONIC, 주제 source의 t_mono_ns)
    uint64_t stream_seq_ = 0;
    bool stream_dirty_ = false;
    worker::TimePoint next_kf_{};
    pid_t helper_ = -1;
    uint64_t helper_restarts_ = 0;
    worker::TimePoint helper_retry_{};
};

}  // namespace source
