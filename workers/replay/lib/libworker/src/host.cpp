#include "worker/host.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>

#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#include <sstream>

#include "worker/child.hpp"
#include "worker/command_server.hpp"
#include "worker/runtime.hpp"

namespace worker {

namespace {

constexpr int kExitConfig = 1;
constexpr int kExitSocket = 3;
constexpr int kExitFailed = 4;
constexpr auto kReportPeriod = std::chrono::seconds(1);
constexpr auto kMaxSleep = std::chrono::milliseconds(100);

const std::set<std::string> kHostKeys = {"watchdog_sec", "stop_timeout_sec", "thread_stall_sec", "command_socket",
                                         "stream_socket", "sim"};
constexpr auto kSimRestart = std::chrono::seconds(2);

}  // namespace

std::string exec_dir() {
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    std::string p(buf, static_cast<size_t>(n));
    const auto slash = p.rfind('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

std::string expand_exec_dir(const std::string& s) {
    static const std::string token = "@EXEC_DIR@";
    std::string out = s;
    for (size_t at = out.find(token); at != std::string::npos; at = out.find(token, at)) {
        const std::string d = exec_dir();
        out.replace(at, token.size(), d);
        at += d.size();
    }
    return out;
}

Json ok_reply(Json data) { return {{"ok", true}, {"data", std::move(data)}}; }

Json error_reply(const std::string& code, const std::string& message) {
    return {{"ok", false}, {"error", {{"code", code}, {"message", message}}}};
}

// ---- Metrics -----------------------------------------------------------------------------------

std::string Metrics::key(const std::string& k) const {
    const std::string p = prefix_ + "_";
    return k.compare(0, p.size(), p) == 0 ? k : p + k;
}

void Metrics::set(const std::string& k, double value, int digits) {
    if (!std::isfinite(value)) return;
    char buf[64];
    if (digits >= 0) std::snprintf(buf, sizeof buf, "%.*f", digits, value);
    else std::snprintf(buf, sizeof buf, "%g", value);
    out_ += (out_.empty() ? "" : " ") + key(k) + "=" + buf;
}

void Metrics::set(const std::string& k, long long value) {
    out_ += (out_.empty() ? "" : " ") + key(k) + "=" + std::to_string(value);
}

void Metrics::set_text(const std::string& k, const std::string& value) {
    out_ += (out_.empty() ? "" : " ") + key(k) + "=" + value;
}

void Metrics::raw(const std::string& text) {
    if (!text.empty()) out_ += (out_.empty() ? "" : " ") + text;
}

// ---- Host --------------------------------------------------------------------------------------

Host::Host(std::string program) : program_(std::move(program)) {}
Host::~Host() = default;

#ifndef WORKER_LIBWORKER_VERSION
#define WORKER_LIBWORKER_VERSION "0"
#endif

const char* Host::libworker_version() { return WORKER_LIBWORKER_VERSION; }

// 설정 파일 → 맨 위(호스트 키 + 모듈 칸). 예전 형식(모듈 칸 이전)의 키는 모듈 칸으로 옮긴다
Json Host::read_config(const std::string& path, std::string* id, long* version) {
    const ConfigFile f = load_config_file(path);
    *id = f.id;
    *version = f.config_version;
    Json cfg = f.config;
    std::string moved;
    for (const auto& [k, mod] : legacy_) {
        const auto it = cfg.find(k);
        if (it == cfg.end()) continue;
        if (!cfg.contains(mod) || !cfg[mod].is_object()) cfg[mod] = Json::object();
        if (!cfg[mod].contains(k)) cfg[mod][k] = *it;
        cfg.erase(k);
        moved += (moved.empty() ? "" : ", ") + k;
    }
    if (!moved.empty())
        std::fprintf(stderr, "%s: old config format: %s read as module keys (save the settings again to update)\n",
                     program_.c_str(), moved.c_str());
    // 맨 위: 호스트 키와 모듈 이름 칸만
    std::set<std::string> known = kHostKeys;
    for (const auto& m : modules_) known.insert(m->name());
    check_keys(cfg, known);
    for (const auto& m : modules_) {
        const auto it = cfg.find(m->name());
        if (it != cfg.end() && !it->is_object()) throw ConfigError("config." + m->name() + " must be an object");
    }
    return cfg;
}

Json Host::host_keys(const Json& cfg) const {
    Json out = Json::object();
    for (const auto& k : kHostKeys)
        if (cfg.contains(k)) out[k] = cfg[k];
    return out;
}

bool Host::configure_file(const std::string& path, std::string* error) {
    try {
        const Json cfg = read_config(path, &id_, &config_version_);
        read(cfg, "watchdog_sec", watchdog_sec_);
        read(cfg, "stop_timeout_sec", stop_timeout_sec_);
        read(cfg, "thread_stall_sec", thread_stall_sec_);
        read(cfg, "command_socket", command_socket_);
        read(cfg, "stream_socket", stream_socket_);
        // 가짜 장치 모드: {"cmd": "프로그램 인자..." 또는 ["프로그램", "인자"...]} (cmd 없이 {}면 표시만: 설정으로 가짜 장치를 씀)
        sim_ = cfg.contains("sim");
        sim_cmd_.clear();
        if (sim_) {
            const Json& s = cfg["sim"];
            require(s.is_object(), "config.sim must be an object");
            check_keys(s, {"cmd"});
            if (s.contains("cmd")) {
                if (s["cmd"].is_string()) {
                    std::istringstream in(s["cmd"].get<std::string>());
                    for (std::string a; in >> a;) sim_cmd_.push_back(expand_exec_dir(a));
                } else if (s["cmd"].is_array()) {
                    for (const auto& a : s["cmd"]) {
                        require(a.is_string(), "config.sim.cmd items must be strings");
                        sim_cmd_.push_back(expand_exec_dir(a.get<std::string>()));
                    }
                } else {
                    throw ConfigError("config.sim.cmd must be a string or a list");
                }
            }
        }
        require(watchdog_sec_ >= 1, "config.watchdog_sec must be >= 1");
        require(stop_timeout_sec_ >= 1, "config.stop_timeout_sec must be >= 1");
        require(thread_stall_sec_ >= 0.5, "config.thread_stall_sec must be >= 0.5");
        if (command_socket_.empty()) command_socket_ = default_command_socket(id_);
        if (stream_socket_.empty()) stream_socket_ = default_stream_socket(id_);
        for (const auto& m : modules_) {
            const auto it = cfg.find(m->name());
            const Json sec = it != cfg.end() ? *it : Json::object();
            m->configure(sec);
            sections_[m->name()] = sec;
        }
        config_path_ = path;
        host_cfg_ = host_keys(cfg);
    } catch (const std::exception& e) {
        *error = e.what();
        return false;
    }
    return true;
}

bool Host::reload_file(const std::string& path, std::string* error) {
    std::string id;
    long version = 0;
    Json cfg;
    try {
        cfg = read_config(path, &id, &version);
    } catch (const std::exception& e) {
        *error = e.what();
        return false;
    }
    if (host_keys(cfg) != host_cfg_) {
        *error = "host keys changed (restart needed)";
        return false;
    }
    std::map<std::string, Json> next = sections_;
    std::string applied;
    for (const auto& m : modules_) {
        const auto it = cfg.find(m->name());
        const Json sec = it != cfg.end() ? *it : Json::object();
        if (sec == sections_[m->name()]) continue;
        bool ok = false;
        try {
            ok = m->reconfigure(sec);
        } catch (const std::exception& e) {
            *error = m->name() + ": " + e.what();
            return false;
        }
        if (!ok) {
            *error = m->name() + ": cannot apply without restart";
            return false;
        }
        next[m->name()] = sec;
        applied += (applied.empty() ? "" : ", ") + m->name();
    }
    sections_ = std::move(next);
    config_version_ = version;
    std::printf("%s: config v%ld applied live (%s)\n", program_.c_str(), version, applied.empty() ? "no change" : applied.c_str());
    return true;
}

void Host::dispatch(const std::string& line, Reply reply) {
    Json req;
    try {
        req = Json::parse(line);
    } catch (const std::exception&) {
    }
    if (!req.is_object() || !req.contains("cmd") || !req["cmd"].is_string())
        return reply(error_reply("bad_request", "request must be {\"cmd\": \"...\", \"args\": {...}}"));
    std::string cmd = req["cmd"].get<std::string>();
    const Json args = req.contains("args") && req["args"].is_object() ? req["args"] : Json::object();
    if (cmd == "get_status") return reply(ok_reply(status()));
    if (const auto a = aliases_.find(cmd); a != aliases_.end()) cmd = a->second;
    // "<모듈>.<명령>". 모듈이 하나뿐이면 앞부분 없이 써도 된다
    std::string mod, sub = cmd;
    if (const auto dot = cmd.find('.'); dot != std::string::npos) {
        mod = cmd.substr(0, dot);
        sub = cmd.substr(dot + 1);
    } else if (modules_.size() == 1) {
        mod = modules_.front()->name();
    }
    for (const auto& m : modules_) {
        if (m->name() != mod) continue;
        try {
            if (m->handle(sub, args, reply)) return;
        } catch (const std::exception& e) {
            return reply(error_reply("internal", e.what()));
        }
        return reply(error_reply("bad_request", "unknown command: " + cmd));
    }
    reply(error_reply("bad_request", "unknown command: " + cmd + " (use <module>.<command>)"));
}

// 가짜 장치 자식: 없거나 끝났으면 kSimRestart 뒤 다시 띄운다(워커는 계속 돈다)
void Host::sim_tick(TimePoint now) {
    if (sim_cmd_.empty()) return;
    if (sim_pid_ > 0) {
        int st = 0;
        if (waitpid(sim_pid_, &st, WNOHANG) != sim_pid_) return;
        std::fprintf(stderr, "%s: %s\n", program_.c_str(), exit_text("sim " + sim_cmd_[0], st).c_str());
        sim_pid_ = -1;
        sim_next_start_ = now + kSimRestart;
        ++sim_restarts_;
        return;
    }
    if (now < sim_next_start_) return;
    sim_pid_ = spawn(sim_cmd_);
    if (sim_pid_ > 0) std::printf("%s: sim device started: %s (pid %d)\n", program_.c_str(), sim_cmd_[0].c_str(), sim_pid_);
    else sim_next_start_ = now + kSimRestart;
}

Json Host::status() const {
    Json out = {{"id", id_}, {"config_version", config_version_}};
    if (sim_) {
        out["sim"] = {{"cmd", sim_cmd_}, {"running", sim_pid_ > 0}, {"restarts", sim_restarts_}};
    }
    for (const auto& m : modules_) out[m->name()] = m->status();
    if (watch_) out["threads"] = watch_->ages_ms(Clock::now());
    return out;
}

std::string Host::metrics() const {
    std::string out;
    for (const auto& m : modules_) {
        Metrics mm(m->name());
        m->metrics(mm);
        if (!mm.str().empty()) out += (out.empty() ? "" : " ") + mm.str();
    }
    return out;
}

int Host::run(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::string config_path;
    bool bad_args = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else bad_args = true;
    }
    if (bad_args || config_path.empty()) {
        std::fprintf(stderr, "usage: %s --config <file>\n", program_.c_str());
        return kExitConfig;
    }
    std::string error;
    if (!configure_file(config_path, &error)) {
        std::fprintf(stderr, "%s: %s\n", program_.c_str(), error.c_str());
        return kExitConfig;
    }

    install_signals();
    arm_watchdog(watchdog_sec_);
    // 가짜 장치는 모듈보다 먼저 띄운다(장치 모듈이 처음부터 붙을 수 있게. 늦어도 모듈은 다시 붙는다)
    sim_tick(Clock::now());
    if (sim_) std::printf("%s: SIM MODE (fake device)%s\n", program_.c_str(), sim_cmd_.empty() ? " (by config)" : "");
    watch_ = std::make_unique<ThreadWatch>(std::chrono::milliseconds(static_cast<int64_t>(thread_stall_sec_ * 1000)));

    // 모듈 시작(붙인 순서). 실패하면 이미 시작한 모듈을 정지하고 끝낸다
    size_t started = 0;
    for (; started < modules_.size(); ++started) {
        try {
            modules_[started]->start(*this);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s: %s\n", program_.c_str(), modules_[started]->name().c_str(), e.what());
            while (started-- > 0) modules_[started]->stop();
            return kExitSocket;
        }
    }

    CommandServer server([&](int conn, const std::string& line) {
        dispatch(line, [&server, conn](const Json& resp) { server.reply(conn, resp.dump()); });
    });
    if (!server.open(command_socket_, &error)) {
        std::fprintf(stderr, "%s: %s\n", program_.c_str(), error.c_str());
        for (size_t i = modules_.size(); i-- > 0;) modules_[i]->stop();
        return kExitSocket;
    }

    std::string names;
    for (const auto& m : modules_) names += (names.empty() ? "" : ", ") + m->name();
    std::printf("%s %s: modules %s, command socket %s\n", program_.c_str(), id_.c_str(), names.c_str(),
                command_socket_.c_str());

    Notifier notify;
    notify.send("READY=1 id=" + id_ + " libworker=" + libworker_version() + (sim_ ? " sim=1" : ""));
    auto next_report = Clock::now();
    std::string stalled_reported;
    int exit_code = 0;
    std::vector<pollfd> fds;
    while (!stop_requested()) {
        if (take_reload_request()) {
            // 바로 적용 설정(SIGHUP): 반영하면 config_version이 바뀌고 다음 heartbeat로 Manager가 확인한다
            if (!reload_file(config_path_, &error))
                std::fprintf(stderr, "%s: config not applied live: %s\n", program_.c_str(), error.c_str());
        }
        const auto now = Clock::now();
        // 스레드 감시: 모두 살아 있을 때만 커널 워치독을 다시 맞춘다(멈춘 스레드가 있으면 워커가 끝난다)
        std::string stalled;
        if (watch_->all_alive(now, &stalled)) {
            arm_watchdog(watchdog_sec_);
            stalled_reported.clear();
        } else if (stalled != stalled_reported) {
            std::fprintf(stderr, "%s: thread stalled: %s (watchdog not refreshed)\n", program_.c_str(), stalled.c_str());
            stalled_reported = stalled;
        }
        std::string why;
        for (const auto& m : modules_) {
            if (m->failed(&why)) {
                std::fprintf(stderr, "%s: %s failed: %s\n", program_.c_str(), m->name().c_str(), why.c_str());
                exit_code = kExitFailed;
                break;
            }
        }
        if (exit_code) break;

        auto wake = std::min(next_report, now + kMaxSleep);
        for (const auto& m : modules_) wake = std::min(wake, m->next_wakeup(now));
        const int timeout_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(wake - now).count());
        fds.clear();
        server.add_pollfds(fds);
        for (const auto& m : modules_) m->add_pollfds(fds);
        const int n = poll(fds.data(), fds.size(), timeout_ms < 0 ? 0 : timeout_ms);
        if (n < 0 && errno != EINTR) {
            std::fprintf(stderr, "%s: poll: %s\n", program_.c_str(), std::strerror(errno));
            break;
        }
        const auto t = Clock::now();
        if (n > 0) {
            server.on_poll(fds);
            for (const auto& m : modules_) m->on_poll(fds, t);
        }
        server.on_timer(t);
        sim_tick(t);
        for (const auto& m : modules_) m->on_timer(t);
        if (t >= next_report) {
            next_report = t + kReportPeriod;
            if (watch_->all_alive(t)) {
                uint64_t progress = 0;
                for (const auto& m : modules_) progress += m->progress();
                const std::string mx = metrics();
                notify.send("WATCHDOG=1 id=" + id_ + " progress=" + std::to_string(progress) +
                            " config_version=" + std::to_string(config_version_) + (mx.empty() ? "" : " " + mx));
            }
        }
    }

    // 종료 절차 (design.md 6.6): 타이머를 정리 상한으로, 명령 소켓 닫기, 모듈 정지(반대 순서)
    arm_watchdog(stop_timeout_sec_);
    std::printf("%s: stopping\n", program_.c_str());
    server.close();
    for (size_t i = modules_.size(); i-- > 0;) modules_[i]->stop();
    if (sim_pid_ > 0) kill_wait(sim_pid_);
    return exit_code;
}

Json call_module(Module& m, const std::string& cmd, const Json& args) {
    auto out = std::make_shared<Json>();  // 늦게 오는 응답이 사라진 지역 변수에 쓰지 않게
    if (!m.handle(cmd, args, [out](const Json& r) { *out = r; })) return error_reply("bad_request", "unknown command");
    return *out;
}

}  // namespace worker
