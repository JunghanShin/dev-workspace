#include "config/module.hpp"

#include <dirent.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

#include "worker/client.hpp"
#include "worker/config.hpp"

namespace config {

namespace {

constexpr int kWaitMs = 100;
const char* kFiles[] = {"workspace.env", "manager.json", "network.conf", "mediamtx.yml"};

int64_t mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

double unix_s() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

std::string env_or(const char* k, const std::string& d) {
    const char* v = std::getenv(k);
    return v && *v ? v : d;
}

bool read_text(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string stamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

Json pm(const Json& req) { return worker::manager_call(req, std::chrono::milliseconds(1500)); }

bool ok(const Json& r) { return r.is_object() && r.value("ok", false); }

}  // namespace

void Module::configure(const worker::Json& config) {
    worker::check_keys(config, {"keep", "root", "data"});
    int keep = 20;
    worker::read(config, "keep", keep);
    worker::require(keep >= 1 && keep <= 1000, "config.keep must be 1..1000");
    keep_ = static_cast<size_t>(keep);
    root_ = env_or("WORKSPACE_ROOT", ".");
    data_ = env_or("WORKSPACE_DATA", ".");
    worker::read(config, "root", root_);
    worker::read(config, "data", data_);
}

void Module::start(worker::Host& host) {
    host_ = &host;
    worker::Publisher::Options po;
    po.thread_name = "config.pub";
    pub_ = std::make_unique<worker::Publisher>(host.watch(), po);
    std::string error;
    if (!pub_->open(host.stream_socket(), &error)) throw std::runtime_error("stream: " + error);
    worker::ManagerSubscription::Options mo;
    mo.topics = {"workers"};
    mo.thread_name = "config.pm";
    pm_ = std::make_unique<worker::ManagerSubscription>(host.watch(), mo);
    pm_->start();
    io_thread_ = std::make_unique<worker::WorkerThread>("config.io", host.watch(), [this](worker::WorkerThread& t) { run_io(t); });
    io_thread_->start();
    // 시작할 때: 버전 목록, 스냅숏 목록, 바뀌었으면 자동 스냅숏
    jobs_.push(Job{0, "versions", Json::object()});
    jobs_.push(Job{0, "snapshot", {{"label", "auto"}}});
}

void Module::run_io(worker::WorkerThread& t) {
    while (!t.stopping()) {
        t.beat();
        auto j = jobs_.pop_for(std::chrono::milliseconds(kWaitMs));
        if (!j) continue;
        done_.push(work(*j));
        t.beat();
    }
}

Json Module::list_snapshots() {
    Json out = Json::array();
    const std::string dir = data_ + "/config/snapshots";
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) names.push_back(n);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        std::string text;
        if (!read_text(dir + "/" + n, text)) continue;
        const Json s = Json::parse(text, nullptr, false);
        if (!s.is_object()) continue;
        out.push_back({{"id", s.value("id", n.substr(0, n.size() - 5))}, {"label", s.value("label", "")}, {"t", s.value("t", 0.0)}});
    }
    return out;
}

Json Module::take_snapshot() {
    Json snap = Json::object();
    Json workers = Json::object();
    const Json list = pm({{"op", "list"}});
    if (!ok(list)) return Json();  // process-manager 없음
    for (const auto& w : list["data"]) {
        const std::string id = w.value("id", "");
        if (id.empty()) continue;
        const Json c = pm({{"op", "get_config"}, {"id", id}});
        if (!ok(c)) continue;
        const Json& d = c["data"];
        workers[id] = {{"preset", d.value("preset", Json())}, {"config", d.value("config", Json::object())},
                       {"temp", d.value("temp", false)}, {"autostart", w.value("autostart", Json())}};
    }
    snap["workers"] = workers;
    const Json conn = pm({{"op", "get_connections"}});
    snap["connections"] = ok(conn) ? Json{{"connections", conn["data"].value("connections", Json())},
                                          {"commanders", conn["data"].value("commanders", Json())}}
                                   : Json();
    Json files = Json::object();
    for (const char* f : kFiles) {
        std::string text;
        if (read_text(data_ + "/config/" + f, text)) files[f] = text;
    }
    snap["files"] = files;
    Json versions = Json::object();
    for (const auto& [name, dir] : bin_dirs(root_)) {
        const Json v = component_version(dir);
        if (!v.is_null()) versions[name] = v;
    }
    snap["versions"] = versions;
    return snap;
}

Module::Done Module::work(const Job& j) {
    Done d{j.id, j.kind, Json(), Json::object()};
    const std::string dir = data_ + "/config/snapshots";
    if (j.kind == "versions") {
        Json versions = Json::object(), mismatch = Json::array();
        for (const auto& [name, bdir] : bin_dirs(root_)) {
            const Json v = component_version(bdir);
            if (v.is_null()) continue;
            versions[name] = v;
            if (!v.value("ok", true)) mismatch.push_back(name);
        }
        std::string prof;
        d.state = {{"versions", versions}, {"mismatch", mismatch},
                   {"profile", read_text(data_ + "/config/profile.json", prof) ? Json::parse(prof, nullptr, false) : Json()}};
        d.result = worker::ok_reply({{"versions", versions}, {"mismatch", mismatch}});
        return d;
    }
    if (j.kind == "snapshots") {
        d.result = worker::ok_reply({{"snapshots", list_snapshots()}});
        return d;
    }
    if (j.kind == "snapshot" || j.kind == "check") {
        Json snap = take_snapshot();
        if (snap.is_null()) {
            d.result = worker::error_reply("unavailable", "process-manager not reachable");
            return d;
        }
        const Json list = list_snapshots();
        Json last;
        if (!list.empty()) {
            std::string text;
            if (read_text(dir + "/" + list.back()["id"].get<std::string>() + ".json", text)) last = Json::parse(text, nullptr, false);
        }
        const bool same = last.is_object() && same_config(last, snap);
        if (j.kind == "check") {
            d.state = {{"drift", !same && last.is_object()}, {"snapshots", list}};
            d.result = worker::ok_reply({{"drift", !same}});
            return d;
        }
        const std::string label = j.args.value("label", "");
        if (label == "auto" && same) {  // 바뀐 것이 없으면 남기지 않음
            d.state = {{"snapshots", list}, {"drift", false}};
            d.result = worker::ok_reply({{"saved", false}});
            return d;
        }
        // 호스트 umask가 엄격해(0177) mkdir 권한에서 실행 비트가 빠진다: 만든 뒤 따로 지정
        for (const std::string& dpath : {data_ + "/config", dir})
            if (::mkdir(dpath.c_str(), 0700) == 0) ::chmod(dpath.c_str(), 0700);
        std::string id = stamp();
        for (int n = 2; access((dir + "/" + id + ".json").c_str(), F_OK) == 0; ++n) id = stamp() + "-" + std::to_string(n);
        snap["id"] = id;
        snap["label"] = label;
        snap["t"] = unix_s();
        const std::string tmp = dir + "/." + id + ".tmp";
        {
            std::ofstream f(tmp);
            f << snap.dump(1);
            if (!f) {
                d.result = worker::error_reply("internal", "cannot write snapshot (disk full?)");
                return d;
            }
        }
        if (std::rename(tmp.c_str(), (dir + "/" + id + ".json").c_str()) != 0) {
            d.result = worker::error_reply("internal", "cannot write snapshot");
            return d;
        }
        std::vector<SnapshotInfo> infos;
        for (const auto& s : list_snapshots()) infos.push_back({s["id"].get<std::string>(), s.value("label", "")});
        for (const auto& old : to_prune(infos, keep_)) std::remove((dir + "/" + old + ".json").c_str());
        d.state = {{"snapshots", list_snapshots()}, {"drift", false},
                   {"event", {{"severity", "info"}, {"code", "snapshot_saved"}, {"text", "snapshot " + id + (label.empty() ? "" : " (" + label + ")")},
                              {"detail", {{"id", id}, {"label", label}}}}}};
        d.result = worker::ok_reply({{"saved", true}, {"id", id}});
        return d;
    }
    // diff·restore: 스냅숏 읽기
    const std::string id = j.args.value("id", "");
    std::string text;
    if (id.empty() || id.find('/') != std::string::npos || !read_text(dir + "/" + id + ".json", text)) {
        d.result = worker::error_reply("not_found", "no snapshot " + id);
        return d;
    }
    const Json snap = Json::parse(text, nullptr, false);
    if (j.kind == "diff") {
        const Json now = take_snapshot();
        if (now.is_null()) {
            d.result = worker::error_reply("unavailable", "process-manager not reachable");
            return d;
        }
        d.result = worker::ok_reply({{"id", id}, {"changes", diff(snap, now)}});
        return d;
    }
    // restore: 워커 설정·연결 선언만(process-manager가 개발 모드로 묶음)
    Json results = Json::object();
    std::set<std::string> only;
    if (j.args.contains("workers") && j.args["workers"].is_array())
        for (const auto& w : j.args["workers"])
            if (w.is_string()) only.insert(w.get<std::string>());
    const Json sw = snap.value("workers", Json::object());
    for (auto it = sw.begin(); it != sw.end(); ++it) {
        if (!only.empty() && !only.count(it.key())) continue;
        const Json cur = pm({{"op", "get_config"}, {"id", it.key()}});
        if (!ok(cur)) {
            results[it.key()] = {{"ok", false}, {"error", cur.value("error", Json("not found"))}};
            continue;
        }
        Json r = {{"ok", true}, {"changed", false}};
        const Json want_preset = it->value("preset", Json());
        if (want_preset.is_string() && cur["data"].value("preset", Json()) != want_preset) {
            const Json u = pm({{"op", "use_preset"}, {"id", it.key()}, {"name", want_preset}});
            if (!ok(u)) {
                results[it.key()] = {{"ok", false}, {"error", u.value("error", Json())}};
                continue;
            }
            r["changed"] = true;
        }
        const Json want = it->value("config", Json::object());
        const Json now = ok(cur) ? pm({{"op", "get_config"}, {"id", it.key()}}) : cur;
        if (!ok(now) || now["data"].value("config", Json::object()) != want) {
            const Json s = pm({{"op", "set_config"}, {"id", it.key()}, {"config", want}});
            if (!ok(s)) r = {{"ok", false}, {"error", s.value("error", Json())}};
            else r["changed"] = true;
        }
        results[it.key()] = r;
    }
    if (only.empty() && snap.contains("connections") && snap["connections"].is_object()) {
        const Json s = pm({{"op", "set_connections"}, {"connections", snap["connections"]}});
        results["(connections)"] = ok(s) ? Json{{"ok", true}} : Json{{"ok", false}, {"error", s.value("error", Json())}};
    }
    d.state = {{"event", {{"severity", "notice"}, {"code", "config_restored"}, {"text", "restored from " + id},
                          {"detail", {{"id", id}, {"results", results}}}}}};
    d.result = worker::ok_reply({{"id", id}, {"results", results}});
    return d;
}

bool Module::push(Job j, worker::Reply reply) {
    j.id = ++next_job_;
    const uint64_t id = j.id;
    if (!jobs_.push(std::move(j))) {
        reply(worker::error_reply("busy", "config-manager is busy"));
        return true;
    }
    replies_[id] = std::move(reply);
    return true;
}

bool Module::handle(const std::string& cmd, const worker::Json& args, worker::Reply reply) {
    const Json a = args.is_object() ? Json::parse(args.dump()) : Json::object();
    if (cmd == "versions") {
        reply(worker::ok_reply({{"versions", versions_}, {"mismatch", mismatch_}, {"libworker", libworker_}, {"profile", profile_}}));
        return true;
    }
    if (cmd == "snapshots") return push(Job{0, "snapshots", a}, std::move(reply));
    if (cmd == "snapshot") return push(Job{0, "snapshot", {{"label", a.value("label", "")}}}, std::move(reply));
    if (cmd == "diff") return push(Job{0, "diff", a}, std::move(reply));
    if (cmd == "restore") return push(Job{0, "restore", a}, std::move(reply));
    return false;
}

void Module::event(const std::string& severity, const std::string& code, const std::string& text, const Json& detail) {
    pending_events_.push_back({{"seq", ++ev_seq_}, {"t_mono_ns", mono_ns()}, {"t_unix", unix_s()}, {"source", "config-manager.config"},
                               {"severity", severity}, {"code", code}, {"text", text}, {"detail", detail}});
    if (pending_events_.size() > 64) pending_events_.erase(pending_events_.begin());
    dirty_ = true;
}

void Module::drain() {
    while (auto ev = pm_ ? pm_->next() : std::nullopt) {
        if (ev->kind != worker::ManagerSubscription::Event::Line || ev->msg.value("event", "") != "worker_status") continue;
        const auto& d = ev->msg["data"];
        const std::string id = d.value("id", "");
        const auto lw = d.contains("libworker") && d["libworker"].is_string() ? d["libworker"].get<std::string>() : std::string();
        if (!id.empty() && libworker_[id] != lw) {
            libworker_[id] = lw;
            dirty_ = true;
        }
    }
    while (auto d = done_.try_pop()) {
        ++jobs_done_;
        const Json& s = d->state;
        if (s.contains("versions")) {
            const Json old = mismatch_;
            versions_ = s["versions"];
            mismatch_ = s["mismatch"];
            profile_ = s.value("profile", Json());
            for (const auto& m : mismatch_)
                if (std::find(old.begin(), old.end(), m) == old.end())
                    event("warning", "build_mismatch", m.get<std::string>() + ": binary differs from BUILD", {{"component", m}});
        }
        if (s.contains("snapshots")) snapshots_ = s["snapshots"];
        if (s.contains("drift")) drift_ = s["drift"].get<bool>();
        if (s.contains("event")) event(s["event"]["severity"], s["event"]["code"], s["event"]["text"], s["event"]["detail"]);
        dirty_ = true;
        auto it = replies_.find(d->id);
        if (it != replies_.end()) {
            it->second(d->result);
            replies_.erase(it);
        }
    }
}

void Module::add_pollfds(std::vector<pollfd>& fds) {
    if (pm_) fds.push_back({pm_->wake_fd(), POLLIN, 0});
    fds.push_back({done_.wake_fd(), POLLIN, 0});
}

void Module::on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) {
    (void)fds;
    drain();
    publish(now);
}

void Module::on_timer(worker::TimePoint now) {
    drain();
    if (now >= next_versions_) {  // 시작 뒤 10분마다(시작 때는 start에서)
        if (next_versions_ != worker::TimePoint{}) jobs_.push(Job{0, "versions", Json::object()});
        next_versions_ = now + std::chrono::minutes(10);
    }
    if (now >= next_check_) {  // 설정이 마지막 스냅숏 뒤로 바뀌었나(drift)
        if (next_check_ != worker::TimePoint{}) jobs_.push(Job{0, "check", Json::object()});
        next_check_ = now + std::chrono::seconds(60);
    }
    publish(now);
}

worker::TimePoint Module::next_wakeup(worker::TimePoint now) const { return now + std::chrono::milliseconds(500); }

void Module::publish(worker::TimePoint now) {
    if (!pub_) return;
    const bool due = now >= next_pub_;
    if (!due && !dirty_) return;
    if (!pub_->has_subscribers()) return;
    dirty_ = false;
    next_pub_ = now + std::chrono::seconds(10);
    const bool kf = pub_->keyframe_requested() || now >= next_kf_;
    if (kf) next_kf_ = now + std::chrono::seconds(30);
    Json versions = Json::object();
    for (auto it = versions_.begin(); it != versions_.end(); ++it)
        versions[it.key()] = {{"commit", it->value("commit", "")}, {"built", it->value("built", "")}, {"ok", it->value("ok", false)}};
    const Json topic = {{"t_mono_ns", mono_ns()}, {"versions", versions}, {"libworker", libworker_}, {"mismatch", mismatch_},
                        {"profile", profile_}, {"snapshots", snapshots_}, {"drift", drift_}};
    Json line = {{"seq", ++seq_}, {"kf", kf}, {"t", unix_s()}, {"topics", {{"config", topic}}}, {"events", pending_events_}, {"events_next", 0}};
    pending_events_.clear();
    pub_->publish(line.dump());
}

worker::Json Module::status() const {
    return {{"snapshots", snapshots_.size()}, {"mismatch", mismatch_}, {"drift", drift_}, {"root", root_}, {"data", data_}};
}

void Module::metrics(worker::Metrics& m) const {
    m.set("snapshots", static_cast<long long>(snapshots_.size()));
    m.set("mismatch", static_cast<long long>(mismatch_.size()));
    m.set("drift", drift_);
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
    if (io_thread_) {
        io_thread_->request_stop();
        io_thread_->join();
        io_thread_.reset();
    }
    for (auto& [id, r] : replies_) r(worker::error_reply("unavailable", "config-manager stopping"));
    replies_.clear();
    if (pm_) {
        pm_->stop();
        pm_.reset();
    }
    if (pub_) {
        pub_->close();
        pub_.reset();
    }
}

}  // namespace config
