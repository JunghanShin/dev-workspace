#include "payload/module.hpp"

#include <poll.h>

#include <chrono>
#include <ctime>
#include <stdexcept>

#include "worker/client.hpp"
#include "worker/config.hpp"

namespace payload {

namespace {

constexpr int kWaitMs = 100;

int64_t mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

double unix_s() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

}  // namespace

void Module::configure(const worker::Json& config) {
    worker::check_keys(config, {"groups"});
    try {
        groups_ = parse_groups(config.contains("groups") ? config["groups"] : Json());
    } catch (const std::invalid_argument& e) {
        throw worker::ConfigError(e.what());
    }
}

void Module::start(worker::Host& host) {
    host_ = &host;
    mods_ = std::make_unique<Modules>(groups_);
    worker::Publisher::Options po;
    po.thread_name = "payload.pub";
    pub_ = std::make_unique<worker::Publisher>(host.watch(), po);
    std::string error;
    if (!pub_->open(host.stream_socket(), &error)) throw std::runtime_error("stream: " + error);
    worker::ManagerSubscription::Options mo;
    mo.topics = {"workers"};
    mo.thread_name = "payload.pm";
    pm_ = std::make_unique<worker::ManagerSubscription>(host.watch(), mo);
    pm_->start();
    // status-manager 주제 system(입력 "system", io.json uses). 이어지지 않았으면 건강을 모르는 채로
    if (const worker::InputAddress* in = host.uses("system")) {
        worker::Subscriber::Options so;
        so.thread_name = "payload.sys";
        so.queue = 16;
        so.socket = in->stream;
        sys_ = std::make_unique<worker::Subscriber>(host.watch(), in->worker, so);
        sys_->start();
    }
    io_thread_ = std::make_unique<worker::WorkerThread>("payload.io", host.watch(), [this](worker::WorkerThread& t) { run_io(t); });
    ctl_thread_ = std::make_unique<worker::WorkerThread>("payload.ctl", host.watch(), [this](worker::WorkerThread& t) { run_ctl(t); });
    io_thread_->start();
    ctl_thread_->start();
}

// 계약 읽기: 워커마다 provides(역할). 막힐 수 있어 코어 밖에서
void Module::run_io(worker::WorkerThread& t) {
    while (!t.stopping()) {
        t.beat();
        auto want = io_want_.pop_for(std::chrono::milliseconds(kWaitMs));
        if (!want) continue;
        std::map<std::string, std::vector<std::string>> got;
        for (const auto& id : *want) {
            if (t.stopping()) break;
            t.beat();
            const Json r = worker::manager_call(Json{{"op", "get_io"}, {"id", id}}, std::chrono::milliseconds(1500));
            std::vector<std::string> roles;
            if (r.is_object() && r.value("ok", false) && r.contains("data") && r["data"].contains("provides") &&
                r["data"]["provides"].is_object())
                for (auto it = r["data"]["provides"].begin(); it != r["data"]["provides"].end(); ++it) roles.push_back(it.key());
            got[id] = roles;
        }
        io_got_.push(std::move(got));
    }
}

// process-manager 쓰기(start·stop). 응답을 결과 큐로
void Module::run_ctl(worker::WorkerThread& t) {
    while (!t.stopping()) {
        t.beat();
        auto req = ctl_q_.pop_for(std::chrono::milliseconds(kWaitMs));
        if (!req) continue;
        Json resp = worker::manager_call(Json{{"op", req->op}, {"id", req->id}}, std::chrono::milliseconds(1500));
        ctl_done_.push(CtlRes{req->job, req->id, req->op, std::move(resp)});
        t.beat();
    }
}

void Module::add_pollfds(std::vector<pollfd>& fds) {
    if (pm_) fds.push_back({pm_->wake_fd(), POLLIN, 0});
    if (sys_) fds.push_back({sys_->wake_fd(), POLLIN, 0});
    fds.push_back({io_got_.wake_fd(), POLLIN, 0});
    fds.push_back({ctl_done_.wake_fd(), POLLIN, 0});
}

void Module::on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) {
    (void)fds;
    drain();
    publish(now);
}

void Module::drain() {
    if (!mods_) return;
    while (auto ev = pm_ ? pm_->next() : std::nullopt) {
        if (ev->kind != worker::ManagerSubscription::Event::Line) continue;
        const std::string e = ev->msg.value("event", "");
        if (e == "worker_status" && ev->msg.contains("data")) {
            const Json d = Json::parse(ev->msg["data"].dump());
            if (!mods_->known(d.value("id", ""))) ids_changed_ = true;
            mods_->on_worker(d);
            dirty_ = true;
        } else if (e == "worker_removed" && ev->msg.contains("data")) {
            mods_->on_removed(ev->msg["data"].value("id", ""));
            dirty_ = true;
        }
    }
    while (auto u = sys_ ? sys_->next() : std::nullopt) {
        if (u->down) {
            mods_->set_health(Json());
        } else if (u->topics.contains("system")) {
            mods_->set_health(u->topics["system"]);
        }
        dirty_ = true;
    }
    while (auto got = io_got_.try_pop()) {
        for (auto& [id, roles] : *got) mods_->set_roles(id, roles);
        dirty_ = true;
    }
    while (auto res = ctl_done_.try_pop()) {
        auto it = jobs_.find(res->job);
        const bool ok = res->response.is_object() && res->response.value("ok", false);
        if (!ok) ++power_failed_;
        const std::string origin = it == jobs_.end() ? "" : it->second.origin;
        if (ok)
            event("info", "module_power", res->id + " " + res->op + " (by " + origin + ")",
                  {{"module", res->id}, {"op", res->op}, {"origin", origin}});
        else
            event("warning", "module_power_failed", res->id + " " + res->op + " failed",
                  {{"module", res->id}, {"op", res->op}, {"origin", origin}, {"error", res->response.value("error", Json())}});
        if (it == jobs_.end()) continue;
        Job& job = it->second;
        job.results[res->id] = ok ? Json{{"ok", true}, {"desired_state", res->response["data"].value("desired_state", Json())}}
                                  : Json{{"ok", false}, {"error", res->response.value("error", Json())}};
        if (--job.remaining > 0) continue;
        if (job.group) {
            job.reply(worker::ok_reply({{"results", job.results}}));
        } else {
            const Json one = job.results.begin().value();
            job.reply(one.value("ok", false) ? worker::ok_reply({{"module", res->id}, {"desired_state", one["desired_state"]}})
                                             : Json{{"ok", false}, {"error", one["error"]}});
        }
        jobs_.erase(it);
    }
}

bool Module::submit(uint64_t job, const std::string& id, bool on) {
    ++power_requests_;
    return ctl_q_.push(CtlReq{job, id, on ? "start" : "stop"});
}

void Module::event(const std::string& severity, const std::string& code, const std::string& text, const Json& detail) {
    pending_events_.push_back({{"seq", ++ev_seq_}, {"t_mono_ns", mono_ns()}, {"t_unix", unix_s()},
                               {"source", "payload-manager.payload"}, {"severity", severity}, {"code", code},
                               {"text", text}, {"detail", detail}});
    if (pending_events_.size() > 64) pending_events_.erase(pending_events_.begin());
    dirty_ = true;
}

bool Module::handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) {
    if (!mods_) return false;
    if (cmd == "list") {
        reply(worker::ok_reply(mods_->snapshot()));
        return true;
    }
    if (cmd != "power" && cmd != "power_group") return false;
    if (!args.is_object() || !args.contains("on") || !args["on"].is_boolean()) {
        reply(worker::error_reply("bad_request", "args need \"on\": true|false"));
        return true;
    }
    const bool on = args["on"].get<bool>();
    std::vector<std::string> targets;
    const bool group = cmd == "power_group";
    if (group) {
        const std::string g = args.value("group", "");
        if (!mods_->has_group(g)) {
            reply(worker::error_reply("bad_request", "no group " + g + " (config payload.groups)"));
            return true;
        }
        targets = mods_->members(g);
        if (targets.empty()) {
            reply(worker::ok_reply({{"results", Json::object()}}));
            return true;
        }
    } else {
        const std::string id = args.value("module", "");
        if (!mods_->known(id)) {
            reply(worker::error_reply("bad_request", "no module " + id));
            return true;
        }
        if (mods_->is_manager(id)) {
            reply(worker::error_reply("forbidden", id + " is a manager (not a module)"));
            return true;
        }
        targets.push_back(id);
    }
    const uint64_t job = ++next_job_;
    Job j;
    j.reply = std::move(reply);
    j.group = group;
    j.origin = host_ ? host_->current_from() : "";
    size_t queued = 0;
    for (const auto& id : targets) {
        if (!submit(job, id, on)) {
            ++power_failed_;
            j.results[id] = {{"ok", false}, {"error", {{"code", "busy"}, {"message", "too many power requests"}}}};
            continue;
        }
        ++queued;
    }
    if (queued == 0) {
        j.reply(group ? worker::ok_reply({{"results", j.results}}) : worker::error_reply("busy", "too many power requests"));
        return true;
    }
    j.remaining = queued;
    jobs_[job] = std::move(j);
    return true;
}

void Module::on_timer(worker::TimePoint now) {
    drain();
    // 입력 연결 알리기(지표 inputs_system_link, get_status inputs). 알리지 않으면 status가 링크 끊김으로 봄
    if (host_ && host_->uses("system")) host_->input_link("system", sys_ && sys_->connected());
    if (mods_ && (ids_changed_ || now >= next_io_)) {
        io_want_.push(mods_->ids());
        ids_changed_ = false;
        next_io_ = now + std::chrono::seconds(10);
    }
    publish(now);
}

worker::TimePoint Module::next_wakeup(worker::TimePoint now) const { return now + std::chrono::milliseconds(200); }

void Module::publish(worker::TimePoint now) {
    if (!pub_ || !mods_) return;
    const bool due = now >= next_pub_;
    if (!due && !dirty_) return;
    if (!pub_->has_subscribers()) return;  // 이벤트는 쌓아 둠(처음 붙는 구독자에게)
    dirty_ = false;
    next_pub_ = now + std::chrono::seconds(1);
    const bool kf = pub_->keyframe_requested() || now >= next_kf_;
    if (kf) next_kf_ = now + std::chrono::seconds(5);
    Json topic = mods_->snapshot();
    topic["t_mono_ns"] = mono_ns();
    Json line = {{"seq", ++seq_}, {"kf", kf}, {"t", unix_s()}, {"topics", {{"payload", topic}}},
                 {"events", pending_events_}, {"events_next", 0}};
    pending_events_.clear();
    pub_->publish(line.dump());
}

worker::Json Module::status() const {
    return {{"modules", mods_ ? mods_->count() : 0}, {"running", mods_ ? mods_->running() : 0},
            {"health_link", sys_ && sys_->connected()}, {"pm_link", pm_ && pm_->connected()}};
}

void Module::metrics(worker::Metrics& m) const {
    m.set("modules", static_cast<long long>(mods_ ? mods_->count() : 0));
    m.set("running", static_cast<long long>(mods_ ? mods_->running() : 0));
    m.set("power_requests", power_requests_);
    m.set("power_failed", power_failed_);
    m.set("pm_link", pm_ && pm_->connected());
    m.set("health_link", sys_ && sys_->connected());
}

bool Module::failed(std::string* why) const {
    for (const worker::WorkerThread* th : {pm_ ? &pm_->thread() : nullptr, pub_ ? &pub_->thread() : nullptr,
                                           sys_ ? &sys_->thread() : nullptr, io_thread_.get(), ctl_thread_.get()})
        if (th && th->failed()) {
            if (why) *why = th->name() + ": " + th->error();
            return true;
        }
    return false;
}

void Module::stop() {
    for (auto& [id, j] : jobs_) j.reply(worker::error_reply("unavailable", "payload-manager stopping"));
    jobs_.clear();
    for (auto* th : {io_thread_.get(), ctl_thread_.get()})
        if (th) {
            th->request_stop();
            th->join();
        }
    io_thread_.reset();
    ctl_thread_.reset();
    if (sys_) {
        sys_->stop();
        sys_.reset();
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

}  // namespace payload
