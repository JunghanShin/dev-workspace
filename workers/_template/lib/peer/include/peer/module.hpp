// peer 모듈: 다른 워커(설정 peer: 워커 id)와 통신하는 본보기 (워크스페이스 design.md 6.6.7).
//
//   큰 데이터   상대의 데이터 버스를 순서대로 전부(InOrder) 받는다      스레드 "peer.data" + framebus::Consumer
//   신호·상태   상대의 스트림을 구독한다                              worker::Subscriber(스레드 "peer.sub")
//   요청·응답   상대에 명령을 보낸다(워커 루프를 막지 않음)            worker::CommandClient(스레드 "peer.cmd")
//
// 상대가 없거나 다시 시작 중이어도 살아서 기다리며 다시 붙는다(지표 peer_link). 상대를 시작·정지하지 않는다.
// peer를 비우면 이 모듈은 아무것도 하지 않는다(생산만 하는 워커).
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "peer/seq_tracker.hpp"
#include "worker/host.hpp"
#include "worker/peer.hpp"
#include "worker/queue.hpp"

namespace peer {

class Module : public worker::Module {
public:
    std::string name() const override { return "peer"; }
    void configure(const worker::Json& config) override;
    void start(worker::Host& host) override;
    void add_pollfds(std::vector<pollfd>& fds) override;
    void on_timer(worker::TimePoint now) override;
    bool handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) override;
    worker::Json status() const override;
    void metrics(worker::Metrics& m) const override;
    uint64_t progress() const override { return tracker_.received(); }
    bool failed(std::string* why) const override;
    void stop() override;

    const std::string& peer_id() const { return peer_; }

private:
    struct Got {  // peer.data 스레드 → 워커 루프
        uint64_t seq;
        size_t bytes;
        float first;
    };
    std::string peer_;
    bool uses_input_ = false;  // 상대 주소를 입력 upstream(host.uses)으로 받음
    std::string bus_socket_;  // 비우면 상대 id의 기본 데이터 버스 소켓
    std::unique_ptr<worker::BoundedQueue<Got>> got_;
    std::unique_ptr<worker::WorkerThread> data_;
    std::unique_ptr<worker::Subscriber> sub_;
    std::unique_ptr<worker::CommandClient> cmd_;
    std::atomic<bool> data_link_{false};
    SeqTracker tracker_;
    size_t last_bytes_ = 0;
    worker::Json last_topics_ = worker::Json::object();
    uint64_t updates_ = 0;
    std::map<uint64_t, worker::Reply> pending_;  // 상대 응답을 기다리는 명령(응답은 정확히 한 번, 나중에)
};

}  // namespace peer
