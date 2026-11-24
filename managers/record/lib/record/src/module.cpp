#include "record/module.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <thread>

#include "worker/config.hpp"
#include "worker/manager_link.hpp"

namespace record {

namespace {

constexpr int kWaitMs = 100;      // 스레드 대기 상한(이보다 오래 막히지 않고 beat)

std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : def;
}

double mono_s() { return mono_ns() / 1e9; }

}  // namespace

Settings read_settings(const worker::Json& config) {
    Settings s;
    const std::string data = env_or("WORKSPACE_DATA", "");
    s.dir = data.empty() ? "records" : data + "/records";
    s.manager_socket = env_or("WORKSPACE_MANAGER_SOCKET", data.empty() ? "manager-logger.sock" : data + "/run/manager-logger.sock");
    worker::check_keys(config, {"dir", "manager_socket", "interval_s", "max_file_mb", "max_total_mb", "fsync_s", "topics", "exclude"});
    worker::read(config, "dir", s.dir);
    worker::read(config, "manager_socket", s.manager_socket);
    worker::read(config, "interval_s", s.interval_s);
    worker::read(config, "max_file_mb", s.max_file_mb);
    worker::read(config, "max_total_mb", s.max_total_mb);
    worker::read(config, "fsync_s", s.fsync_s);
    if (config.contains("topics")) s.topics = Json::parse(config["topics"].dump());
    if (config.contains("exclude")) s.exclude = Json::parse(config["exclude"].dump());
    try {
        TopicRules check;
        check.configure(s.topics, s.exclude);
    } catch (const std::invalid_argument& e) {
        throw worker::ConfigError(e.what());
    }
    worker::require(!s.dir.empty(), "record.dir must not be empty");
    worker::require(s.interval_s > 0, "record.interval_s must be > 0");
    worker::require(s.max_file_mb > 0 && s.max_total_mb >= s.max_file_mb, "record.max_total_mb must be >= max_file_mb > 0");
    worker::require(s.fsync_s >= 0, "record.fsync_s must be >= 0");
    return s;
}

Module::Module() = default;
Module::~Module() { stop(); }

void Module::configure(const worker::Json& config) {
    s_ = read_settings(config);
    rules_.configure(s_.topics, s_.exclude);
}

void Module::start(worker::Host& host) {
    host_ = &host;
    boot_ = boot_id();
    many_ = std::make_unique<worker::ManyInputs>("", host.id());
    platform_ = std::make_unique<PlatformRecorder>(s_.interval_s);
    file_ = std::make_unique<RecordFile>(s_.dir, static_cast<uint64_t>(s_.max_file_mb * 1048576),
                                         static_cast<uint64_t>(s_.max_total_mb * 1048576), s_.fsync_s, boot_);
    {
        std::lock_guard<std::mutex> lock(info_mu_);
        path_ = file_->path();
    }
    Json start = Json::object();
    start["type"] = "logger";
    start["event"] = "start";
    start["pid"] = static_cast<long long>(::getpid());
    put(start);

    auto& watch = host.watch();
    write_thread_ = std::make_unique<worker::WorkerThread>("record.write", watch, [this](worker::WorkerThread& t) { run_write(t); });
    find_thread_ = std::make_unique<worker::WorkerThread>("record.find", watch, [this](worker::WorkerThread& t) { run_find(t); });
    worker::ManagerSubscription::Options po;
    po.socket = s_.manager_socket;
    po.topics = {"workers", "system", "commands"};
    po.period_ms = static_cast<int>(s_.interval_s * 1000);
    po.retry_sec = 2.0;  // 예전 logger와 같음
    po.thread_name = "record.pm";
    pm_ = std::make_unique<worker::ManagerSubscription>(watch, po);
    write_thread_->start();
    find_thread_->start();
    pm_->start();
    sync_subs();
}

void Module::put(const Json& rec) { put(rec, mono_ns()); }

void Module::put(const Json& rec, int64_t t_mono_ns) {
    std::string line = make_line(rec, now_iso(), t_mono_ns);
    if (!write_q_.push(std::move(line))) drops_.fetch_add(1);
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
        rules_.forget(it->first);
        it = subs_.erase(it);
    }
    for (const auto& id : want) {
        if (subs_.count(id)) continue;
        worker::Subscriber::Options o;
        o.thread_name = ("rs." + id).substr(0, 15);
        o.retry_sec = 1.0;
        auto sub = std::make_unique<worker::Subscriber>(host_->watch(), id, o);
        sub->start();
        subs_[id] = std::move(sub);
    }
}

void Module::on_timer(worker::TimePoint now) {
    (void)now;
    sync_subs();
}

void Module::add_pollfds(std::vector<pollfd>& fds) {
    if (pm_) fds.push_back({pm_->wake_fd(), POLLIN, 0});
    fds.push_back({found_q_.wake_fd(), POLLIN, 0});
    for (const auto& [id, sub] : subs_) fds.push_back({sub->wake_fd(), POLLIN, 0});
}

void Module::on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) {
    (void)fds;
    (void)now;
    drain();
}

void Module::drain() {
    while (auto ev = pm_ ? pm_->next() : std::nullopt) {
        switch (ev->kind) {
        case worker::ManagerSubscription::Event::Connected:
            pm_link_ = true;
            put(platform_->connected());
            break;
        case worker::ManagerSubscription::Event::Down:
            pm_link_ = false;
            for (const auto& r : platform_->disconnected(ev->reason)) put(r);
            break;
        case worker::ManagerSubscription::Event::Line:
            for (const auto& r : platform_->handle(ev->msg, mono_s())) put(r);
            break;
        }
    }
    for (auto& [id, sub] : subs_) {
        while (auto u = sub->next()) {
            if (u->down) {
                rules_.forget(id);  // 다시 붙으면 kf부터 바로 남김
                continue;
            }
            const Json topics = Json::parse(u->topics.dump()), events = Json::parse(u->events.dump());
            for (auto& r : rules_.records(id, topics, events, u->kf, mono_ns())) {
                (r.rec["type"] == "event" ? events_ : topics_) += 1;
                put(r.rec, r.t_mono_ns);
            }
        }
    }
    while (auto res = found_q_.try_pop()) {
        auto it = pending_.find(res->id);
        if (it == pending_.end()) continue;
        it->second(worker::ok_reply(worker::Json::parse(res->result.dump())));
        pending_.erase(it);
    }
}

bool Module::handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) {
    if (cmd != "find") return false;
    const uint64_t id = next_find_++;
    FindReq req{id, Json::parse(args.is_object() ? args.dump() : "{}")};
    if (!find_q_.push(std::move(req))) {
        reply(worker::error_reply("busy", "too many find requests"));
        return true;
    }
    pending_[id] = std::move(reply);
    return true;
}

void Module::run_write(worker::WorkerThread& t) {
    double next_df = 0;
    auto write_one = [this](const std::string& line) {
        std::string err;
        if (file_->write(line, &err)) {
            lines_.fetch_add(1);
            bytes_.fetch_add(line.size());
        } else {
            write_errors_.fetch_add(1);
            drops_.fetch_add(1);
            std::lock_guard<std::mutex> lock(info_mu_);
            last_error_ = err;
        }
        std::lock_guard<std::mutex> lock(info_mu_);
        path_ = file_->path();
    };
    while (!t.stopping()) {
        t.beat();
        if (auto line = write_q_.pop_for(std::chrono::milliseconds(kWaitMs))) write_one(*line);
        const double now = mono_s();
        if (now >= next_df) {
            struct statvfs v {};
            if (::statvfs(s_.dir.c_str(), &v) == 0)
                disk_free_mb_ = static_cast<long long>(static_cast<double>(v.f_bavail) * v.f_frsize / 1048576);
            next_df = now + 1.0;
        }
    }
    while (auto line = write_q_.try_pop()) write_one(*line);  // 끝날 때 남은 줄을 모두 쓴다
    file_->close();
}

void Module::run_find(worker::WorkerThread& t) {
    while (!t.stopping()) {
        t.beat();
        auto req = find_q_.pop_for(std::chrono::milliseconds(kWaitMs));
        if (!req) continue;
        Json result = find(s_.dir, req->args, boot_);
        finds_.fetch_add(1);
        found_q_.push(FindResult{req->id, std::move(result)});
    }
}

worker::Json Module::status() const {
    std::lock_guard<std::mutex> lock(info_mu_);
    return worker::Json{{"file", path_},           {"lines", lines_.load()}, {"bytes", bytes_.load()},
                        {"drops", drops_.load()},  {"pm_link", pm_link_.load()},
                        {"last_error", last_error_.empty() ? worker::Json() : worker::Json(last_error_)}};
}

void Module::metrics(worker::Metrics& m) const {
    m.set("lines", lines_.load());
    m.set("bytes", bytes_.load());
    m.set("drops", drops_.load());
    m.set("files", file_ ? file_->files_opened() : uint64_t{0});
    m.set("write_errors", write_errors_.load());
    m.set("pm_link", pm_link_.load());
    m.set("disk_free_mb", disk_free_mb_.load());
    m.set("finds", finds_.load());
    int up = 0;
    for (const auto& [id, sub] : subs_) up += sub->connected() ? 1 : 0;
    m.set("subs", static_cast<long long>(subs_.size()));
    m.set("subs_up", up);
    m.set("topics", topics_);
    m.set("events", events_);
}

bool Module::failed(std::string* why) const {
    if (pm_ && pm_->thread().failed()) {
        if (why) *why = pm_->thread().name() + ": " + pm_->thread().error();
        return true;
    }
    for (const auto* th : {write_thread_.get(), find_thread_.get()}) {
        if (th && th->failed()) {
            if (why) *why = th->name() + ": " + th->error();
            return true;
        }
    }
    return false;
}

void Module::stop() {
    for (auto& [id, sub] : subs_) sub->stop();
    if (platform_) drain();  // 구독에서 이미 받은 것도 남긴다
    subs_.clear();
    if (pm_) {
        pm_->stop();
    }
    if (find_thread_) {
        find_thread_->request_stop();
        find_thread_->join();
        find_thread_.reset();
    }
    if (write_thread_) {
        if (platform_) drain();  // 구독에서 이미 받은 것도 남긴다
        Json stop = Json::object();
        stop["type"] = "logger";
        stop["event"] = "stop";
        put(stop);
        write_thread_->request_stop();
        write_thread_->join();  // 남은 줄을 쓰고 닫는다
        write_thread_.reset();
    }
    for (auto& [id, reply] : pending_) reply(worker::error_reply("unavailable", "stopping"));
    pending_.clear();
}

}  // namespace record
