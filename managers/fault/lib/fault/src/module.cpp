#include "fault/module.hpp"

#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <set>
#include <stdexcept>

#include "worker/config.hpp"

namespace fault {

namespace {

int64_t mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

double unix_s() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

}  // namespace

void Module::configure(const worker::Json& config) {
    worker::check_keys(config, {"rules"});
    try {
        rules_ = parse_rules(config.contains("rules") ? Json::parse(config["rules"].dump()) : Json());
    } catch (const std::invalid_argument& e) {
        throw worker::ConfigError(e.what());
    }
}

void Module::start(worker::Host& host) {
    host_ = &host;
    engine_ = std::make_unique<Engine>(rules_);
    engine_->set_system(Json(), mono_ns());  // 아직 status를 모름(3 s 안에 안 오면 status_lost)
    worker::Publisher::Options po;
    po.thread_name = "fault.pub";
    pub_ = std::make_unique<worker::Publisher>(host.watch(), po);
    std::string error;
    if (!pub_->open(host.stream_socket(), &error)) throw std::runtime_error("stream: " + error);
    worker::ManagerSubscription::Options mo;
    mo.topics = {"workers", "system"};
    mo.thread_name = "fault.pm";
    pm_ = std::make_unique<worker::ManagerSubscription>(host.watch(), mo);
    pm_->start();
    if (const worker::InputAddress* in = host.uses("system")) {
        worker::Subscriber::Options so;
        so.thread_name = "fault.sys";
        so.queue = 16;
        so.socket = in->stream;
        sys_ = std::make_unique<worker::Subscriber>(host.watch(), in->worker, so);
        sys_->start();
    }
    if (const worker::InputAddress* in = host.uses("mission")) {
        worker::CommandClient::Options co;
        co.thread_name = "fault.cmd";
        co.queue = 16;
        co.timeout_ms = 3000;
        co.socket = in->command;
        mission_ = std::make_unique<worker::CommandClient>(host.watch(), in->worker, co);
        mission_->start();
    }
    many_ = std::make_unique<worker::ManyInputs>("", host.id());
    sync_subs();
}

void Module::sync_subs() {
    if (!many_ || !host_ || !many_->refresh()) return;
    std::set<std::string> want;
    for (const auto& id : many_->workers("streams")) want.insert(id);
    for (auto it = subs_.begin(); it != subs_.end();) {
        if (want.count(it->first)) {
            ++it;
            continue;
        }
        it->second->stop();
        it = subs_.erase(it);
    }
    for (const auto& id : want) {
        if (subs_.count(id)) continue;
        worker::Subscriber::Options o;
        o.thread_name = ("fs." + id).substr(0, 15);
        o.queue = 64;
        auto sub = std::make_unique<worker::Subscriber>(host_->watch(), id, o);
        sub->start();
        subs_[id] = std::move(sub);
    }
}

void Module::add_pollfds(std::vector<pollfd>& fds) {
    if (pm_) fds.push_back({pm_->wake_fd(), POLLIN, 0});
    if (sys_) fds.push_back({sys_->wake_fd(), POLLIN, 0});
    if (mission_) fds.push_back({mission_->wake_fd(), POLLIN, 0});
    for (const auto& [id, s] : subs_) fds.push_back({s->wake_fd(), POLLIN, 0});
}

void Module::on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) {
    (void)fds;
    drain();
    publish(now);
}

void Module::drain() {
    if (!engine_) return;
    const int64_t now = mono_ns();
    while (auto ev = pm_ ? pm_->next() : std::nullopt) {
        if (ev->kind != worker::ManagerSubscription::Event::Line) continue;
        const std::string e = ev->msg.value("event", "");
        if (e == "worker_status" && ev->msg.contains("data")) {
            const auto& d = ev->msg["data"];
            if (d.contains("metrics")) engine_->set_metrics(d.value("id", ""), Json::parse(d["metrics"].dump()));
        } else if (e == "worker_removed" && ev->msg.contains("data")) {
            engine_->remove_worker(ev->msg["data"].value("id", ""));
        } else if (e == "system" && ev->msg.contains("data") && ev->msg["data"].contains("access_mode")) {
            access_mode_ = ev->msg["data"].value("access_mode", "operation");
        }
    }
    while (auto u = sys_ ? sys_->next() : std::nullopt) {
        if (u->down) engine_->set_system(Json(), now);
        else if (u->topics.contains("system")) engine_->set_system(Json::parse(u->topics["system"].dump()), now);
    }
    for (auto& [id, sub] : subs_)
        while (auto u = sub->next())
            if (!u->down)
                for (const auto& e : u->events) engine_->on_event(id, Json::parse(e.dump()), now);
    while (auto rep = mission_ ? mission_->next() : std::nullopt) {
        auto it = sent_.find(rep->id);
        if (it == sent_.end()) continue;
        const bool ok = rep->response.is_object() && rep->response.value("ok", false);
        const Json err = ok ? Json() : Json::parse(rep->response.value("error", worker::Json()).dump());
        engine_->action_result(it->second.first, it->second.second, ok, err);
        if (!ok) {
            ++action_failed_;
            event({{"severity", "warning"}, {"code", "fault_action_failed"},
                   {"text", it->second.first + ": action failed"}, {"detail", {{"fault", it->second.first}, {"error", err}}}});
        }
        sent_.erase(it);
        dirty_ = true;
    }
}

void Module::event(const Json& e) {
    Json full = e;
    full["seq"] = ++ev_seq_;
    full["t_mono_ns"] = mono_ns();
    full["t_unix"] = unix_s();
    full["source"] = "fault-manager.fault";
    pending_events_.push_back(full);
    if (pending_events_.size() > 64) pending_events_.erase(pending_events_.begin());
    dirty_ = true;
}

void Module::evaluate() {
    ++ticks_;
    auto out = engine_->tick(mono_ns(), access_mode_ != "development");
    for (const auto& e : out.events) {
        if (e.value("code", "") == "fault_raised") ++raised_;
        event(e);
    }
    for (const auto& s : out.sends) {
        ++actions_;
        uint64_t id = 0;
        if (mission_)
            id = mission_->send("mission.send", {{"target", s.action.target}, {"cmd", s.action.cmd}, {"args", s.action.args}});
        if (id == 0) {
            ++action_failed_;
            const Json err = {{"code", "unavailable"}, {"message", mission_ ? "command queue full" : "no mission input"}};
            engine_->action_result(s.fault, s.index, false, err);
            event({{"severity", "warning"}, {"code", "fault_action_failed"}, {"text", s.fault + ": action failed"},
                   {"detail", {{"fault", s.fault}, {"error", err}}}});
            continue;
        }
        sent_[id] = {s.fault, s.index};
    }
}

void Module::on_timer(worker::TimePoint now) {
    // 입력 연결 알리기(지표 inputs_<입력>_link). mission은 명령 소켓이 있으면 붙은 것으로(명령은 요청마다 연결)
    if (host_) {
        if (host_->uses("system")) host_->input_link("system", sys_ && sys_->connected());
        if (const worker::InputAddress* in = host_->uses("mission"))
            host_->input_link("mission", mission_ && (mission_->reachable() || ::access(in->command.c_str(), F_OK) == 0));
    }
    if (now >= next_sync_) {
        sync_subs();
        next_sync_ = now + std::chrono::seconds(1);
    }
    drain();
    if (engine_ && now >= next_eval_) {
        evaluate();
        next_eval_ = now + std::chrono::milliseconds(200);
    }
    publish(now);
}

worker::TimePoint Module::next_wakeup(worker::TimePoint now) const { return now + std::chrono::milliseconds(100); }

void Module::publish(worker::TimePoint now) {
    if (!pub_ || !engine_) return;
    const bool due = now >= next_pub_;
    if (!due && !dirty_) return;
    if (!pub_->has_subscribers()) return;  // 이벤트는 쌓아 둠(처음 붙는 구독자에게)
    dirty_ = false;
    next_pub_ = now + std::chrono::seconds(1);
    const bool kf = pub_->keyframe_requested() || now >= next_kf_;
    if (kf) next_kf_ = now + std::chrono::seconds(5);
    Json topic = engine_->snapshot(mono_ns());
    topic["t_mono_ns"] = mono_ns();
    Json line = {{"seq", ++seq_}, {"kf", kf}, {"t", unix_s()}, {"topics", {{"fault", topic}}},
                 {"events", pending_events_}, {"events_next", 0}};
    pending_events_.clear();
    pub_->publish(line.dump());
}

bool Module::handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) {
    if (!engine_) return false;
    if (cmd == "list") {
        reply(worker::ok_reply(engine_->snapshot(mono_ns())));
        return true;
    }
    if (cmd == "ack") {
        const std::string id = args.is_object() ? args.value("id", "") : "";
        if (!engine_->ack(id)) {
            reply(worker::error_reply("not_found", "no active fault " + id));
            return true;
        }
        dirty_ = true;
        reply(worker::ok_reply({{"id", id}, {"acked", true}}));
        return true;
    }
    return false;
}

worker::Json Module::status() const {
    return {{"rules", rules_.size()}, {"active", engine_ ? engine_->active() : 0}, {"worst", engine_ ? engine_->worst() : "none"},
            {"access_mode", access_mode_}, {"mission_link", mission_ != nullptr}, {"status_link", sys_ && sys_->connected()}};
}

void Module::metrics(worker::Metrics& m) const {
    m.set("active", static_cast<long long>(engine_ ? engine_->active() : 0));
    m.set("worst", engine_ ? severity_rank(engine_->worst()) : 0);
    m.set("raised", raised_);
    m.set("actions", actions_);
    m.set("action_failed", action_failed_);
    m.set("pm_link", pm_ && pm_->connected());
}

bool Module::failed(std::string* why) const {
    for (const worker::WorkerThread* th : {pm_ ? &pm_->thread() : nullptr, pub_ ? &pub_->thread() : nullptr,
                                           sys_ ? &sys_->thread() : nullptr, mission_ ? &mission_->thread() : nullptr})
        if (th && th->failed()) {
            if (why) *why = th->name() + ": " + th->error();
            return true;
        }
    return false;
}

void Module::stop() {
    for (auto& [id, s] : subs_) s->stop();
    subs_.clear();
    for (auto* s : {sys_.get()})
        if (s) s->stop();
    sys_.reset();
    if (mission_) {
        mission_->stop();
        mission_.reset();
    }
    if (pm_) {
        pm_->stop();
        pm_.reset();
    }
    if (pub_) {
        pub_->close();
        pub_.reset();
    }
}

}  // namespace fault
