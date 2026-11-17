// processor 모듈: 같은 워커의 source 모듈이 만든 표본을 포트(SourcePort)로 받아 유닛 Window로 처리한다.
// 워커 안 연결의 본보기: 이 모듈은 source의 포트 헤더(source/port.hpp)만 안다. 같은 포트를 구현한 다른 모듈로 바꿔 끼울 수 있다.
#pragma once

#include "processor/window.hpp"
#include "source/port.hpp"
#include "worker/host.hpp"

namespace processor {

class Module : public worker::Module {
public:
    explicit Module(source::SourcePort& src) : src_(src) {}  // 다른 모듈의 포트는 생성자로 받는다
    std::string name() const override { return "processor"; }
    void configure(const worker::Json& config) override;
    bool reconfigure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return win_.total(); }

    void attach();                             // 포트 구독(start가 부른다. 시험에서는 호스트 없이 직접)
    void on_sample(const source::Sample& s);  // 포트 콜백

private:
    source::SourcePort& src_;
    Window win_;
    uint64_t last_seq_ = 0;
};

}  // namespace processor
