// 송출 모듈 "stream": 장치 워커 안의 영상 송출(Streams, 자식 프로세스)을 워커 모듈로 감싼다
// (워커 모듈 구조, 워크스페이스 design.md 6.6.3). 송출하는 장치 워커가 같이 붙인다:
//
//     auto& cam = host.add<camera::Module>();
//     host.add<video::StreamsModule>([&cam] { return cam.stream_defaults(); });   // 입력 기본값은 장치 모듈이 준다
//
//   설정 칸 "stream": {"streams": [{"name", "url", "mode", ...}]}   (칸 하나하나는 lib/libvideo/stream-schema.json)
//   명령 "stream.start" / "stream.stop" {"name"}   (예전 이름 stream_start / stream_stop은 워커가 host.alias로)
//   상태 get_status "stream" 칸: 송출 목록 [{name, link, enabled, fps, ...}]
//   지표 stream_<이름>_link / _fps / _restarts
//
// 기본값 함수는 configure 때 부른다. 장치 모듈을 먼저 add하면(먼저 configure됨) 그 설정을 쓸 수 있다.
#pragma once

#include <functional>

#include "video/stream.hpp"
#include "worker/host.hpp"

namespace video {

class StreamsModule : public worker::Module {
public:
    using Defaults = std::function<StreamConfig()>;
    explicit StreamsModule(Defaults defaults = {}) : defaults_(std::move(defaults)) {}

    std::string name() const override { return "stream"; }
    void configure(const worker::Json& config) override;
    void add_pollfds(std::vector<pollfd>& fds) override { streams_.add_pollfds(fds); }
    void on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) override { streams_.on_poll(fds, now); }
    void on_timer(worker::TimePoint now) override {
        streams_.on_timer(now);
        // 끊김·복구 이벤트를 호스트로(스트림을 내는 모듈이 발행, libworker 0.18 Host::event)
        if (host())
            for (const auto& e : streams_.take_events())
                host()->event(name(), e.value("severity", "notice"), e.value("code", ""), e.value("text", ""), e.value("detail", worker::Json::object()));
    }
    worker::TimePoint next_wakeup(worker::TimePoint now) const override { return streams_.next_wakeup(now); }
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    worker::Json status() const override { return streams_.status(); }
    void metrics(worker::Metrics& m) const override;
    void stop() override { streams_.stop(); }

    Streams& streams() { return streams_; }

private:
    Defaults defaults_;
    Streams streams_;
};

}  // namespace video
