#include "record/module.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "worker/config.hpp"

namespace record {

namespace {

constexpr int kWaitMs = 100;      // 스레드 대기 상한(이보다 오래 막히지 않고 beat)
constexpr double kRetryS = 2.0;   // process-manager에 다시 붙는 간격(예전 logger와 같음)

std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : def;
}

double env_num(const char* name, double def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    return end && *end == '\0' ? d : def;
}

double mono_s() { return mono_ns() / 1e9; }

int connect_unix(const std::string& path, std::string* why) {
    sockaddr_un addr{};
    if (path.size() >= sizeof(addr.sun_path)) {
        *why = "socket path too long";
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *why = std::strerror(errno);
        return -1;
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        *why = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

// 정지 요청을 보며 seconds 동안 쉰다(beat 계속)
void nap(worker::WorkerThread& t, double seconds) {
    const double until = mono_s() + seconds;
    while (!t.stopping() && mono_s() < until) {
        t.beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(kWaitMs));
    }
}

}  // namespace

Settings read_settings(const worker::Json& config) {
    Settings s;
    const std::string data = env_or("WORKSPACE_DATA", "");
    s.dir = env_or("WORKSPACE_LOGGER_DIR", data.empty() ? "records" : data + "/records");
    s.manager_socket = env_or("WORKSPACE_MANAGER_SOCKET", data.empty() ? "manager-logger.sock" : data + "/run/manager-logger.sock");
    s.interval_s = env_num("WORKSPACE_LOGGER_INTERVAL_S", s.interval_s);
    s.max_file_mb = env_num("WORKSPACE_LOGGER_MAX_FILE_MB", s.max_file_mb);
    s.max_total_mb = env_num("WORKSPACE_LOGGER_MAX_TOTAL_MB", s.max_total_mb);
    worker::check_keys(config, {"dir", "manager_socket", "interval_s", "max_file_mb", "max_total_mb", "fsync_s"});
    worker::read(config, "dir", s.dir);
    worker::read(config, "manager_socket", s.manager_socket);
    worker::read(config, "interval_s", s.interval_s);
    worker::read(config, "max_file_mb", s.max_file_mb);
    worker::read(config, "max_total_mb", s.max_total_mb);
    worker::read(config, "fsync_s", s.fsync_s);
    worker::require(!s.dir.empty(), "record.dir must not be empty");
    worker::require(s.interval_s > 0, "record.interval_s must be > 0");
    worker::require(s.max_file_mb > 0 && s.max_total_mb >= s.max_file_mb, "record.max_total_mb must be >= max_file_mb > 0");
    worker::require(s.fsync_s >= 0, "record.fsync_s must be >= 0");
    return s;
}

Module::Module() = default;
Module::~Module() { stop(); }

void Module::configure(const worker::Json& config) { s_ = read_settings(config); }

void Module::start(worker::Host& host) {
    boot_ = boot_id();
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
    pm_thread_ = std::make_unique<worker::WorkerThread>("record.pm", watch, [this](worker::WorkerThread& t) { run_pm(t); });
    write_thread_->start();
    find_thread_->start();
    pm_thread_->start();
}

void Module::put(const Json& rec) {
    std::string line = make_line(rec, now_iso(), mono_ns());
    if (!write_q_.push(std::move(line))) drops_.fetch_add(1);
}

void Module::add_pollfds(std::vector<pollfd>& fds) {
    fds.push_back({pm_q_.wake_fd(), POLLIN, 0});
    fds.push_back({found_q_.wake_fd(), POLLIN, 0});
}

void Module::on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) {
    (void)fds;
    (void)now;
    drain();
}

void Module::drain() {
    while (auto ev = pm_q_.try_pop()) {
        switch (ev->kind) {
        case PmEvent::Connected:
            pm_link_ = true;
            put(platform_->connected());
            break;
        case PmEvent::Down:
            pm_link_ = false;
            for (const auto& r : platform_->disconnected(ev->reason)) put(r);
            break;
        case PmEvent::Line:
            for (const auto& r : platform_->handle(ev->msg, mono_s())) put(r);
            break;
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

void Module::run_pm(worker::WorkerThread& t) {
    const double read_timeout = std::max(5.0, s_.interval_s * 5);
    while (!t.stopping()) {
        std::string why;
        const int fd = connect_unix(s_.manager_socket, &why);
        if (fd < 0) {
            pm_q_.push(PmEvent{PmEvent::Down, Json(), why});
            nap(t, kRetryS);
            continue;
        }
        Json sub = Json::object();
        sub["op"] = "subscribe";
        sub["topics"] = Json::array({"workers", "system", "commands"});
        sub["period_ms"] = static_cast<long long>(s_.interval_s * 1000);
        const std::string req = sub.dump() + "\n";
        (void)!::send(fd, req.data(), req.size(), MSG_NOSIGNAL);

        std::string buf, reason = "manager closed the subscription";
        bool first = true;
        double last_rx = mono_s();
        char chunk[65536];
        while (!t.stopping()) {
            t.beat();
            pollfd p{fd, POLLIN, 0};
            const int n = ::poll(&p, 1, kWaitMs);
            if (n < 0 && errno != EINTR) {
                reason = std::strerror(errno);
                break;
            }
            if (n <= 0) {
                if (mono_s() - last_rx > read_timeout) {
                    reason = "timed out";
                    break;
                }
                continue;
            }
            const ssize_t r = ::recv(fd, chunk, sizeof chunk, 0);
            if (r <= 0) break;
            last_rx = mono_s();
            buf.append(chunk, static_cast<size_t>(r));
            size_t pos;
            bool stop_session = false;
            while ((pos = buf.find('\n')) != std::string::npos) {
                const std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                Json msg = Json::parse(line, nullptr, false);
                if (first) {
                    first = false;
                    if (!msg.is_object() || !msg.value("ok", false)) {
                        reason = "subscribe refused: " + (msg.is_object() && msg.contains("error") ? msg["error"].dump() : line);
                        stop_session = true;
                        break;
                    }
                    pm_q_.push(PmEvent{PmEvent::Connected, Json(), ""});
                    continue;
                }
                if (msg.is_discarded()) continue;  // 깨진 줄은 건너뜀(예전 logger와 같음)
                pm_q_.push(PmEvent{PmEvent::Line, std::move(msg), ""});
            }
            if (stop_session) break;
        }
        ::close(fd);
        if (t.stopping()) break;
        pm_q_.push(PmEvent{PmEvent::Down, Json(), reason});
        nap(t, kRetryS);
    }
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
}

bool Module::failed(std::string* why) const {
    for (const auto* th : {pm_thread_.get(), write_thread_.get(), find_thread_.get()}) {
        if (th && th->failed()) {
            if (why) *why = th->name() + ": " + th->error();
            return true;
        }
    }
    return false;
}

void Module::stop() {
    if (pm_thread_) {
        pm_thread_->request_stop();
        pm_thread_->join();
        pm_thread_.reset();
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
