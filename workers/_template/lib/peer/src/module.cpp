#include "peer/module.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "framebus/framebus.hpp"
#include "worker/workspace.hpp"

namespace peer {

using worker::Json;

void Module::configure(const Json& c) {
    worker::check_keys(c, {"peer", "bus_socket"});
    worker::read(c, "peer", peer_);
    worker::read(c, "bus_socket", bus_socket_);
    if (!peer_.empty() && host() && peer_ == host()->id())
        throw worker::ConfigError("config.peer.peer must be another worker (not itself)");
}

void Module::start(worker::Host& host) {
    if (peer_.empty()) return;  // 상대가 없으면 꺼짐
    const std::string bus = bus_socket_.empty() ? worker::default_socket(peer_, "frames") : bus_socket_;
    got_ = std::make_unique<worker::BoundedQueue<Got>>(256, worker::Overflow::DropOldest);
    // 큰 데이터: 상대 데이터 버스를 순서대로 전부 받는다. 끊기면 기다렸다 다시 붙는다
    data_ = std::make_unique<worker::WorkerThread>("peer.data", host.watch(), [this, bus](worker::WorkerThread& t) {
        framebus::Consumer c(bus, framebus::Consumer::Mode::InOrder);
        while (!t.stopping()) {
            if (!c.connected()) {
                std::string err;
                data_link_ = c.connect(&err);
                if (!data_link_) {
                    for (int i = 0; i < 10 && !t.stopping(); ++i) {  // 1초 기다리며 beat
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        t.beat();
                    }
                    continue;
                }
            }
            framebus::Frame f;
            const auto r = c.next(100, &f);
            if (r == framebus::Consumer::Result::Frame) {
                float first = 0;
                if (f.bytes >= sizeof(float)) std::memcpy(&first, f.data, sizeof(float));
                got_->push(Got{f.frame_id, f.bytes, first});  // 처리(여기서는 요약만)를 워커 루프로
                c.release(f);
            } else if (r == framebus::Consumer::Result::Disconnected) {
                data_link_ = false;
            }
            t.beat();
        }
    });
    data_->start();
    // 신호·상태: 상대 스트림 구독
    worker::Subscriber::Options so;
    so.thread_name = "peer.sub";
    sub_ = std::make_unique<worker::Subscriber>(host.watch(), peer_, so);
    sub_->start();
    // 요청·응답: 상대에 명령(비동기)
    worker::CommandClient::Options co;
    co.thread_name = "peer.cmd";
    cmd_ = std::make_unique<worker::CommandClient>(host.watch(), peer_, co);
    cmd_->start();
    std::printf("peer: %s (data bus %s)\n", peer_.c_str(), bus.c_str());
}

void Module::add_pollfds(std::vector<pollfd>& fds) {
    if (!got_) return;
    fds.push_back({got_->wake_fd(), POLLIN, 0});
    fds.push_back({sub_->wake_fd(), POLLIN, 0});
    fds.push_back({cmd_->wake_fd(), POLLIN, 0});
}

void Module::on_timer(worker::TimePoint) {
    if (!got_) return;
    while (auto g = got_->try_pop()) {
        tracker_.on(g->seq);
        last_bytes_ = g->bytes;
    }
    while (auto u = sub_->next()) {
        ++updates_;
        if (u->down) continue;
        if (u->kf) last_topics_ = u->topics;
        else last_topics_.update(u->topics);
    }
    while (auto r = cmd_->next()) {
        auto it = pending_.find(r->id);
        if (it != pending_.end()) {
            it->second(r->response);  // 상대의 응답을 그대로 돌려준다
            pending_.erase(it);
        }
    }
}

bool Module::handle(const std::string& cmd, const Json& args, worker::Reply reply) {
    if (cmd == "set_rate") {  // 상대(source 모듈)의 생성 주기를 바꾼다: 워커 간 요청·응답
        if (!cmd_) {
            reply(worker::error_reply("unavailable", "no peer configured"));
            return true;
        }
        const uint64_t id = cmd_->send("source.set_rate", args);
        if (id == 0) {
            reply(worker::error_reply("unavailable", "command queue full"));
            return true;
        }
        pending_.emplace(id, std::move(reply));  // 응답은 on_timer에서(나중에, 한 번)
        return true;
    }
    if (cmd == "reset") {
        tracker_.reset();
        reply(worker::ok_reply({{"received", 0}}));
        return true;
    }
    return false;
}

Json Module::status() const {
    return {{"peer", peer_},
            {"data_link", data_link_.load()},
            {"stream_link", sub_ ? sub_->connected() : false},
            {"received", tracker_.received()},
            {"lost", tracker_.lost()},
            {"last_seq", tracker_.last()},
            {"peer_restarts", tracker_.restarts()},
            {"last_bytes", last_bytes_},
            {"stream_updates", updates_},
            {"peer_topics", last_topics_}};
}

void Module::metrics(worker::Metrics& m) const {
    if (peer_.empty()) return;
    m.set("link", data_link_.load() && sub_ && sub_->connected());  // 공통 이름: _link
    m.set("received", tracker_.received());
    m.set("lost", tracker_.lost());
}

bool Module::failed(std::string* why) const {
    const worker::WorkerThread* ts[] = {data_.get(), sub_ ? &sub_->thread() : nullptr, cmd_ ? &cmd_->thread() : nullptr};
    for (const auto* t : ts) {
        if (t && t->failed()) {
            *why = t->name() + ": " + t->error();
            return true;
        }
    }
    return false;
}

void Module::stop() {
    if (data_) {
        data_->request_stop();
        data_->join();
    }
    if (sub_) sub_->stop();
    if (cmd_) cmd_->stop();
    for (auto& [id, reply] : pending_) reply(worker::error_reply("unavailable", "stopping"));
    pending_.clear();
}

}  // namespace peer
