#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include "mission/module.hpp"

#include <poll.h>

#include <chrono>
#include <ctime>
#include <set>
#include <stdexcept>

#include "worker/client.hpp"
#include "worker/config.hpp"

namespace mission {

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
    worker::check_keys(config, {"queue", "command_timeout_ms", "total_timeout_ms", "idle_close_s"});
    Settings s;
    int queue = static_cast<int>(s.queue);
    worker::read(config, "queue", queue);
    worker::read(config, "command_timeout_ms", s.command_timeout_ms);
    worker::read(config, "total_timeout_ms", s.total_timeout_ms);
    worker::read(config, "idle_close_s", s.idle_close_s);
    worker::require(queue >= 0 && queue <= 64, "mission.queue must be 0..64");
    worker::require(s.command_timeout_ms >= 100, "mission.command_timeout_ms must be >= 100");
    worker::require(s.total_timeout_ms >= s.command_timeout_ms, "mission.total_timeout_ms must be >= command_timeout_ms");
    worker::require(s.idle_close_s > 0, "mission.idle_close_s must be > 0");
    s.queue = static_cast<size_t>(queue);
    s_ = s;
}

void Module::start(worker::Host& host) {
    if (const char* pf = std::getenv("WORKSPACE_PEERS_FILE"); pf && *pf) peers_file_ = pf;
    host_ = &host;
    disp_ = std::make_unique<Dispatcher>(s_.queue, static_cast<int64_t>(s_.total_timeout_ms) * 1000000);
    worker::Publisher::Options po;
    po.thread_name = "mission.pub";
    pub_ = std::make_unique<worker::Publisher>(host.watch(), po);
    std::string error;
    if (!pub_->open(host.stream_socket(), &error)) throw std::runtime_error("stream: " + error);
    worker::ManagerSubscription::Options mo;
    mo.topics = {"workers"};
    mo.thread_name = "mission.pm";
    pm_ = std::make_unique<worker::ManagerSubscription>(host.watch(), mo);
    pm_->start();
    io_thread_ = std::make_unique<worker::WorkerThread>("mission.io", host.watch(), [this](worker::WorkerThread& t) { run_io(t); });
    io_thread_->start();
}

// 계약 읽기: 코어가 준 워커 목록마다 Manager API get_io(막힐 수 있어 코어 밖에서)
void Module::run_io(worker::WorkerThread& t) {
    while (!t.stopping()) {
        t.beat();
        auto want = io_want_.pop_for(std::chrono::milliseconds(kWaitMs));
        if (!want) continue;
        std::map<std::string, Contract> got;
        for (const auto& id : *want) {
            if (t.stopping()) break;
            t.beat();
            const Json r = worker::manager_call(Json{{"op", "get_io"}, {"id", id}}, std::chrono::milliseconds(1500));
            if (r.is_object() && r.value("ok", false) && r.contains("data")) got[id] = contract_from_io(r["data"]);
            else got[id] = Contract{};  // 계약 없음: 모든 명령이 self
        }
        io_got_.push(std::move(got));
    }
}

void Module::add_pollfds(std::vector<pollfd>& fds) {
    if (pm_) fds.push_back({pm_->wake_fd(), POLLIN, 0});
    fds.push_back({io_got_.wake_fd(), POLLIN, 0});
    for (const auto& [k, l] : lanes_)
        if (l.client) fds.push_back({l.client->wake_fd(), POLLIN, 0});
    for (const auto& [k, l] : fast_)
        if (l.client) fds.push_back({l.client->wake_fd(), POLLIN, 0});
}

void Module::on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) {
    (void)fds;
    drain();
    publish(now, false);
}

void Module::drain() {
    while (auto ev = pm_ ? pm_->next() : std::nullopt) {
        if (ev->kind != worker::ManagerSubscription::Event::Line) continue;
        const std::string e = ev->msg.value("event", "");
        if (e == "worker_status" && ev->msg.contains("data")) {
            const auto& d = ev->msg["data"];
            const std::string id = d.value("id", "");
            if (id.empty()) continue;
            if (!states_.count(id)) ids_changed_ = true;
            states_[id] = d.value("actual_state", "");
        } else if (e == "worker_removed" && ev->msg.contains("data")) {
            states_.erase(ev->msg["data"].value("id", ""));
            ids_changed_ = true;
        }
    }
    while (auto got = io_got_.try_pop())
        for (auto& [id, c] : *got) contracts_[id] = std::move(c);
    for (auto& [key, lane] : lanes_) {
        if (!lane.client) continue;
        while (auto rep = lane.client->next()) {
            if (rep->id != lane.client_id) continue;  // 지난 것(시간 초과 뒤 늦게 온 응답 등)
            const uint64_t rid = lane.request_id;
            lane.client_id = lane.request_id = 0;
            const Json resp = rep->response;
            const std::string code = resp.is_object() && resp.contains("error") ? resp["error"].value("code", "") : "";
            if (code == "timeout") {
                ++timeouts_;
                event("warning", "command_timeout", key + ": no answer", {{"lane", key}});
            }
            finish(rid, resp);
            dirty_ = true;
            if (auto next = disp_->done(key)) send_now(*next);
        }
    }
    for (auto& [target, lane] : fast_) {
        if (!lane.client) continue;
        while (auto rep = lane.client->next()) {
            auto it = lane.pending.find(rep->id);
            if (it == lane.pending.end()) continue;
            const uint64_t rid = it->second;
            lane.pending.erase(it);
            finish(rid, rep->response);
            dirty_ = true;
        }
    }
}

void Module::send_now(const Request& r) {
    Lane& lane = lanes_[r.key()];
    if (!lane.client) {
        worker::CommandClient::Options o;
        o.thread_name = ("mc." + r.key()).substr(0, 15);
        o.timeout_ms = s_.command_timeout_ms;
        o.queue = 2;
        o.via_entry = false;  // 입구 자신: 워커로 바로
        lane.client = std::make_unique<worker::CommandClient>(host_->watch(), r.target, o);
        lane.client->start();
    }
    lane.last_used = worker::Clock::now();
    Json envelope = Json::object();
    if (!r.origin.empty()) envelope["origin"] = r.origin;
    const uint64_t cid = lane.client->send(r.cmd, r.args, envelope);
    if (cid == 0) {  // 큐가 참(일어나지 않아야 함: 한 번에 하나)
        finish(r.id, worker::error_reply("busy", r.key() + ": command thread busy"));
        if (auto next = disp_->done(r.key())) send_now(*next);
        return;
    }
    lane.client_id = cid;
    lane.request_id = r.id;
    ++sent_;
    dirty_ = true;
}

void Module::send_fast(const Request& r) {
    FastLane& lane = fast_[r.target];
    if (!lane.client) {
        worker::CommandClient::Options o;
        o.thread_name = ("mf." + r.target).substr(0, 15);
        o.timeout_ms = 1000;
        o.queue = 8;
        o.via_entry = false;
        lane.client = std::make_unique<worker::CommandClient>(host_->watch(), r.target, o);
        lane.client->start();
    }
    lane.last_used = worker::Clock::now();
    Json envelope = Json::object();
    if (!r.origin.empty()) envelope["origin"] = r.origin;
    const uint64_t cid = lane.client->send(r.cmd, r.args, envelope);
    if (cid == 0) {
        ++busy_;
        finish(r.id, worker::error_reply("busy", r.target + ": fast lane full"));
        return;
    }
    lane.pending[cid] = r.id;
    ++fast_sent_;
    dirty_ = true;
}

void Module::refresh_access() {
    if (peers_file_.empty()) return;
    struct stat st{};
    if (::stat(peers_file_.c_str(), &st) != 0) return;
    const long long m = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    if (m == peers_mtime_ns_) return;
    try {
        std::ifstream in(peers_file_);
        access_ = access_from_peers(Json::parse(in));
        peers_mtime_ns_ = m;
    } catch (const std::exception&) {
        // 쓰는 중이었을 수 있음: 다음 명령 때 다시(지난 표 그대로)
    }
}

void Module::finish(uint64_t request_id, const Json& response) {
    auto it = replies_.find(request_id);
    if (it == replies_.end()) return;
    it->second(response);
    replies_.erase(it);
}

void Module::event(const std::string& severity, const std::string& code, const std::string& text, const Json& detail) {
    pending_events_.push_back({{"seq", ++ev_seq_}, {"t_mono_ns", mono_ns()}, {"t_unix", unix_s()},
                               {"source", "mission-manager.mission"}, {"severity", severity}, {"code", code},
                               {"text", text}, {"detail", detail}});
    if (pending_events_.size() > 64) pending_events_.erase(pending_events_.begin());
    dirty_ = true;
}

bool Module::handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) {
    if (cmd == "get_queues") {
        reply(worker::ok_reply({{"queues", disp_ ? disp_->snapshot(mono_ns()) : Json::object()},
                                {"sent", sent_}, {"busy", busy_}, {"timeouts", timeouts_}}));
        return true;
    }
    if (cmd != "send") return false;
    if (!args.is_object() || !args.contains("target") || !args["target"].is_string() || !args.contains("cmd") ||
        !args["cmd"].is_string()) {
        reply(worker::error_reply("bad_request", "args must be {\"target\": \"<worker>\", \"cmd\": \"...\", \"args\": {...}}"));
        return true;
    }
    Request r;
    r.target = args["target"].get<std::string>();
    r.cmd = args["cmd"].get<std::string>();
    r.args = args.contains("args") && args["args"].is_object() ? args["args"] : Json::object();
    r.origin = host_ ? host_->current_from() : "";
    if (r.target == (host_ ? host_->id() : "")) {
        reply(worker::error_reply("bad_request", "mission cannot send to itself"));
        return true;
    }
    auto st = states_.find(r.target);
    if (st != states_.end() && st->second != "running") {
        ++unavailable_;
        reply(worker::error_reply("unavailable", r.target + " is not running (" + st->second + ")"));
        return true;
    }
    auto c = contracts_.find(r.target);
    const Contract* contract = c == contracts_.end() ? nullptr : &c->second;
    refresh_access();
    if (check_access(access_, r.origin, r.target, r.cmd, r.args, contract) == Verdict::Deny) {
        ++forbidden_;
        dirty_ = true;
        const auto now = worker::Clock::now();
        auto& last = denied_at_[r.origin + "\x1f" + r.target + "." + r.cmd];
        if (last == worker::TimePoint{} || now - last >= std::chrono::seconds(10)) {
            last = now;
            event("warning", "command_forbidden", r.origin + " may not send " + r.cmd + " to " + r.target,
                  {{"origin", r.origin}, {"target", r.target}, {"cmd", r.cmd}});
        }
        reply(worker::error_reply("forbidden", r.origin + " is not allowed to send " + r.cmd + " to " + r.target));
        return true;
    }
    r.role = role_of(contract, r.cmd);
    r.id = ++next_id_;
    r.enq_ns = mono_ns();
    if (fast_command(contract, r.cmd)) {  // 큐를 기다리지 않음(external-link.md 5장)
        replies_[r.id] = std::move(reply);
        send_fast(r);
        return true;
    }
    switch (disp_->submit(r)) {
    case Dispatcher::Admit::Busy:
        ++busy_;
        event("warning", "command_busy", r.key() + ": " + r.cmd + " from " + r.origin + " refused (queue full)",
              {{"lane", r.key()}, {"cmd", r.cmd}, {"origin", r.origin}});
        reply(worker::error_reply("busy", r.key() + ": " + std::to_string(s_.queue) + " commands waiting"));
        return true;
    case Dispatcher::Admit::Queued:
        replies_[r.id] = std::move(reply);
        dirty_ = true;
        return true;
    case Dispatcher::Admit::Send:
        replies_[r.id] = std::move(reply);
        send_now(r);
        return true;
    }
    return true;
}

void Module::on_timer(worker::TimePoint now) {
    drain();
    if (disp_)
        for (const auto& r : disp_->expire(mono_ns())) {
            ++timeouts_;
            event("warning", "command_timeout", r.key() + ": " + r.cmd + " waited too long", {{"lane", r.key()}, {"cmd", r.cmd}});
            finish(r.id, worker::error_reply("timeout", r.key() + ": waited too long in queue"));
        }
    // 쓰지 않는 명령 스레드 닫기
    for (auto it = lanes_.begin(); it != lanes_.end();) {
        if (it->second.client_id == 0 && disp_->idle(it->first) &&
            now - it->second.last_used > std::chrono::duration<double>(s_.idle_close_s)) {
            it->second.client->stop();
            it = lanes_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = fast_.begin(); it != fast_.end();) {
        if (it->second.pending.empty() && now - it->second.last_used > std::chrono::duration<double>(s_.idle_close_s)) {
            it->second.client->stop();
            it = fast_.erase(it);
        } else {
            ++it;
        }
    }
    // 계약 다시 읽기: 워커가 늘거나 줄면 바로, 아니면 10 s마다
    if (ids_changed_ || now >= next_io_) {
        std::vector<std::string> ids;
        for (const auto& [id, s] : states_)
            if (!host_ || id != host_->id()) ids.push_back(id);
        if (!ids.empty() || ids_changed_) io_want_.push(std::move(ids));
        ids_changed_ = false;
        next_io_ = now + std::chrono::seconds(10);
    }
    publish(now, false);
}

worker::TimePoint Module::next_wakeup(worker::TimePoint now) const { return now + std::chrono::milliseconds(100); }

void Module::publish(worker::TimePoint now, bool force) {
    if (!pub_ || !disp_) return;
    const bool due = force || now >= next_pub_;
    if (!due && !dirty_) return;
    if (!pub_->has_subscribers()) return;  // 이벤트는 쌓아 둠(처음 붙는 구독자에게)
    dirty_ = false;
    next_pub_ = now + std::chrono::seconds(1);
    const bool kf = pub_->keyframe_requested() || now >= next_kf_;
    if (kf) next_kf_ = now + std::chrono::seconds(5);
    const Json topic = {{"t_mono_ns", mono_ns()}, {"queues", disp_->snapshot(mono_ns())}, {"sent", sent_}, {"busy", busy_},
                        {"timeouts", timeouts_}, {"unavailable", unavailable_}, {"forbidden", forbidden_}};
    Json line = {{"seq", ++seq_}, {"kf", kf}, {"t", unix_s()}, {"topics", {{"mission", topic}}},
                 {"events", pending_events_}, {"events_next", 0}};
    pending_events_.clear();
    pub_->publish(line.dump());
}

worker::Json Module::status() const {
    return {{"sent", sent_}, {"busy", busy_}, {"timeouts", timeouts_}, {"lanes", lanes_.size()},
            {"contracts", contracts_.size()}, {"pm_link", pm_ && pm_->connected()}};
}

void Module::metrics(worker::Metrics& m) const {
    m.set("sent", sent_);
    m.set("busy", busy_);
    m.set("timeouts", timeouts_);
    m.set("unavailable", unavailable_);
    m.set("forbidden", forbidden_);
    m.set("fast_sent", fast_sent_);
    m.set("queues", static_cast<long long>(lanes_.size()));
    m.set("waiting", static_cast<long long>(disp_ ? disp_->waiting() : 0));
    m.set("pm_link", pm_ && pm_->connected());
}

bool Module::failed(std::string* why) const {
    for (const worker::WorkerThread* th : {pm_ ? &pm_->thread() : nullptr, pub_ ? &pub_->thread() : nullptr, io_thread_.get()})
        if (th && th->failed()) {
            if (why) *why = th->name() + ": " + th->error();
            return true;
        }
    return false;
}

void Module::stop() {
    for (auto& [id, reply] : replies_) reply(worker::error_reply("unavailable", "mission-manager stopping"));
    replies_.clear();
    for (auto& [k, l] : lanes_)
        if (l.client) l.client->stop();
    lanes_.clear();
    for (auto& [k, l] : fast_)
        if (l.client) l.client->stop();
    fast_.clear();
    if (io_thread_) {
        io_thread_->request_stop();
        io_thread_->join();
        io_thread_.reset();
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

}  // namespace mission
