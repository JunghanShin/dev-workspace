#include "status/module.hpp"

#include <poll.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <set>
#include <stdexcept>

#include "worker/config.hpp"

namespace status {

namespace {

int64_t mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

double unix_s() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string read_boot_id() {
    std::ifstream f("/proc/sys/kernel/random/boot_id");
    std::string s;
    std::getline(f, s);
    std::string out;
    for (char c : s)
        if (c != '-') out += c;
    return out.size() >= 8 ? out.substr(0, 8) : "noboot00";
}

}  // namespace

void Module::configure(const worker::Json& config) {
    try {
        rules_ = parse_rules(config);
    } catch (const std::invalid_argument& e) {
        throw worker::ConfigError(e.what());
    }
    publish_ms_ = 1000;
    worker::read(config, "publish_ms", publish_ms_);
    worker::require(publish_ms_ >= 100, "status.publish_ms must be >= 100");
}

void Module::start(worker::Host& host) {
    host_ = &host;
    boot_id_ = read_boot_id();
    health_ = std::make_unique<Health>(rules_);
    worker::Publisher::Options po;
    po.thread_name = "status.pub";
    pub_ = std::make_unique<worker::Publisher>(host.watch(), po);
    std::string error;
    if (!pub_->open(host.stream_socket(), &error)) throw std::runtime_error("stream: " + error);
    worker::ManagerSubscription::Options mo;
    mo.topics = {"workers", "system"};
    mo.period_ms = 1000;
    mo.thread_name = "status.pm";
    pm_ = std::make_unique<worker::ManagerSubscription>(host.watch(), mo);
    pm_->start();
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
        health_->forget_stream(it->first);
        it = subs_.erase(it);
        dirty_ = true;
    }
    for (const auto& id : want) {
        if (subs_.count(id)) continue;
        worker::Subscriber::Options o;
        o.thread_name = ("ss." + id).substr(0, 15);
        o.queue = 16;
        auto sub = std::make_unique<worker::Subscriber>(host_->watch(), id, o);
        sub->start();
        health_->set_stream(id, false);
        subs_[id] = std::move(sub);
        dirty_ = true;
    }
}

void Module::add_pollfds(std::vector<pollfd>& fds) {
    if (pm_) fds.push_back({pm_->wake_fd(), POLLIN, 0});
    for (const auto& [id, sub] : subs_) fds.push_back({sub->wake_fd(), POLLIN, 0});
}

void Module::on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) {
    (void)fds;
    drain(now);
    publish(now, false);
}

void Module::drain(worker::TimePoint now) {
    (void)now;
    if (!health_) return;
    while (auto ev = pm_ ? pm_->next() : std::nullopt) {
        switch (ev->kind) {
        case worker::ManagerSubscription::Event::Connected:
            health_->set_pm_link(true);
            dirty_ = true;
            break;
        case worker::ManagerSubscription::Event::Down:
            health_->set_pm_link(false);
            dirty_ = true;
            break;
        case worker::ManagerSubscription::Event::Line: {
            const std::string e = ev->msg.value("event", "");
            if (e == "worker_status" && ev->msg.contains("data")) {
                health_->on_worker(Json::parse(ev->msg["data"].dump()), mono_ns());
                dirty_ = true;
            } else if (e == "worker_removed" && ev->msg.contains("data") && ev->msg["data"].contains("id")) {
                health_->on_removed(ev->msg["data"]["id"].get<std::string>());
                dirty_ = true;
            }
            break;
        }
        }
    }
    for (auto& [id, sub] : subs_) {
        bool seen = false;
        while (sub->next()) seen = true;  // 이번 판은 붙었는지만 본다(design.md 2.4)
        (void)seen;
        health_->set_stream(id, sub->connected());
    }
}

Json Module::system_now() {
    const int64_t now = mono_ns();
    auto res = health_->evaluate(now);
    res.system["clock"] = {{"t_mono_ns", now}, {"t_unix", unix_s()}, {"boot_id", boot_id_}};
    for (auto& e : res.events) {
        e["t_unix"] = unix_s();
        pending_events_.push_back(std::move(e));
    }
    return res.system;
}

// 건강이 바뀌면 바로, 그대로면 publish_ms마다(구독자가 오래됨을 알아보게). heartbeat 나이 같은 잦은 값만 바뀐 것은 주기에
void Module::publish(worker::TimePoint now, bool force) {
    if (!health_) return;
    const bool due = force || now >= next_pub_;
    if (!due && !dirty_) return;
    dirty_ = false;
    Json sys = system_now();
    Json sig = {{"h", sys["health"]}, {"pm", sys["pm_link"]}, {"w", Json::object()}};
    for (auto it = sys["workers"].begin(); it != sys["workers"].end(); ++it)
        sig["w"][it.key()] = {(*it)["health"], (*it)["stream"], (*it)["required"]};
    const bool changed = sig != signature_;
    signature_ = sig;
    current_ = sys;
    if (!due && !changed && pending_events_.empty()) return;
    next_pub_ = now + std::chrono::milliseconds(publish_ms_);
    if (!pub_ || !pub_->has_subscribers()) {
        pending_events_.clear();  // 받을 곳이 없으면 버림
        return;
    }
    const bool kf = pub_->keyframe_requested() || now >= next_kf_;
    if (kf) next_kf_ = now + std::chrono::seconds(5);
    Json line = {{"seq", ++seq_}, {"kf", kf}, {"t", unix_s()}, {"topics", {{"system", sys}}},
                 {"events", pending_events_}, {"events_next", 0}};
    pending_events_.clear();
    pub_->publish(line.dump());
    ++published_;
}

void Module::on_timer(worker::TimePoint now) {
    if (now >= next_sync_) {
        sync_subs();
        next_sync_ = now + std::chrono::seconds(1);
    }
    drain(now);
    publish(now, false);
}

worker::TimePoint Module::next_wakeup(worker::TimePoint now) const { return now + std::chrono::milliseconds(200); }

bool Module::handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) {
    (void)args;
    if (cmd != "get_system") return false;
    reply(worker::ok_reply(current_.empty() ? system_now() : current_));
    return true;
}

worker::Json Module::status() const {
    return {{"health", current_.value("health", Json())}, {"pm_link", current_.value("pm_link", false)},
            {"subs", subs_.size()}};
}

void Module::metrics(worker::Metrics& m) const {
    const std::string h = current_.value("health", std::string("ok"));
    m.set("health", h == "fault" ? 2 : h == "degraded" ? 1 : 0);
    m.set("workers", static_cast<long long>(current_.contains("workers") ? current_["workers"].size() : 0));
    m.set("faults", static_cast<long long>(current_.contains("counts") ? current_["counts"].value("fault", 0) : 0));
    m.set("pm_link", pm_ && pm_->connected());
    int up = 0;
    for (const auto& [id, sub] : subs_) up += sub->connected() ? 1 : 0;
    m.set("subs", static_cast<long long>(subs_.size()));
    m.set("subs_up", up);
}

bool Module::failed(std::string* why) const {
    if (pm_ && pm_->thread().failed()) {
        if (why) *why = pm_->thread().name() + ": " + pm_->thread().error();
        return true;
    }
    if (pub_ && pub_->thread().failed()) {
        if (why) *why = pub_->thread().name() + ": " + pub_->thread().error();
        return true;
    }
    return false;
}

void Module::stop() {
    for (auto& [id, sub] : subs_) sub->stop();
    subs_.clear();
    if (pm_) {
        pm_->stop();
        pm_.reset();
    }
    if (pub_) {
        pub_->close();
        pub_.reset();
    }
}

}  // namespace status
