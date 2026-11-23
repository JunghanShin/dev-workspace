#include "worker/host.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
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
                                         "stream_socket", "sim", "stages", "loop_budget_ms"};
constexpr int kMaxConsecutiveErrors = 3;  // on_timer·on_poll 연속 예외가 이만큼이면 상태가 깨졌다고 본다
constexpr size_t kMaxWarnings = 10;
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
        stages_cfg_ = Json::object();
        if (cfg.contains("stages")) {
            require(cfg["stages"].is_object(), "config.stages must be an object {\"<module>.<role>\": {...}}");
            stages_cfg_ = cfg["stages"];
            for (const auto& [name, v] : stages_cfg_.items()) parse_stage_options(v, name, StageOptions{});
        }
        read(cfg, "loop_budget_ms", loop_budget_ms_);
        require(loop_budget_ms_ >= 1 && loop_budget_ms_ <= 1000, "config.loop_budget_ms must be 1..1000");
        const char* fi = std::getenv("WORKER_FAULT_INJECTION");
        fault_enabled_ = fi && std::string(fi) == "1";
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

void Host::set_peers_file(const std::string& path) { cmd_guard_ = std::make_unique<CommandGuard>(id_, path); }

void Host::dispatch(const std::string& line, Reply reply, int peer_pid) {
    Json req;
    try {
        req = Json::parse(line);
    } catch (const std::exception&) {
    }
    if (!req.is_object() || !req.contains("cmd") || !req["cmd"].is_string())
        return reply(error_reply("bad_request", "request must be {\"cmd\": \"...\", \"args\": {...}}"));
    std::string cmd = req["cmd"].get<std::string>();
    const Json args = req.contains("args") && req["args"].is_object() ? req["args"] : Json::object();
    if (const auto a = aliases_.find(cmd); a != aliases_.end()) cmd = a->second;
    // 명령하는 곳 검사(워크스페이스 docs/command-path.md 5장 ②): 허용 목록 밖이면 warn은 기록만, enforce는 거절
    const std::string claimed = req.contains("from") && req["from"].is_string() ? req["from"].get<std::string>() : "";
    CommandGuard::Decision g;
    if (cmd_guard_) g = cmd_guard_->check(peer_pid, cmd, claimed);
    else {
        g.read = cmd == "get_status";
        g.from = claimed;
    }
    if (g.violation) {
        const std::string key = g.from + std::string("\x1f") + cmd;
        const auto now = Clock::now();
        const auto it = guard_logged_.find(key);
        if (it == guard_logged_.end() || now - it->second > std::chrono::seconds(60)) {
            guard_logged_[key] = now;
            if (guard_logged_.size() > 256) guard_logged_.clear();
            std::fprintf(stderr, "%s: command guard (%s): %s from %s: %s\n", program_.c_str(), g.run ? "warn" : "denied",
                         cmd.c_str(), g.from.c_str(), g.reason.c_str());
        }
        if (!g.run) return reply(error_reply("forbidden", "not allowed: " + g.reason));
    }
    if (!g.read) {  // 마지막 명령(get_status 공통 칸 last_command): 응답이 오면 ok를 채운다
        const auto t = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        last_command_ = {{"cmd", cmd}, {"from", g.from.empty() ? Json(nullptr) : Json(g.from)}, {"t_mono_ns", t}, {"ok", nullptr}};
        reply = [this, t, inner = std::move(reply)](const Json& resp) {
            if (last_command_.is_object() && last_command_.value("t_mono_ns", int64_t(0)) == t)
                last_command_["ok"] = resp.is_object() && resp.value("ok", false);
            inner(resp);
        };
    }
    if (cmd == "get_status") return reply(ok_reply(status()));
    if (cmd == "__fault") {
        if (!fault_enabled_) return reply(error_reply("bad_request", "unknown command: __fault"));
        std::string err;
        if (!fault(args, &err)) return reply(error_reply("bad_request", err));
        return reply(ok_reply({{"injected", args}}));
    }
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
        } catch (const Fatal& e) {
            fatal_ = m->name() + ".handle: " + e.what();
            return reply(error_reply("internal", e.what()));
        } catch (const std::exception& e) {
            note_error(m->name() + ".handle(" + sub + "): " + e.what());  // 명령 하나의 오류는 그 명령에서 끝
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
    for (const auto& m : modules_) {
        try {
            out[m->name()] = m->status();
        } catch (const std::exception& e) {
            out[m->name()] = {{"error", std::string("status: ") + e.what()}};
        }
    }
    if (watch_) out["threads"] = watch_->ages_ms(Clock::now());
    out["last_command"] = last_command_.is_object() ? last_command_ : Json(nullptr);
    out["runtime"] = runtime_status();
    return out;
}

std::string Host::metrics() const {
    std::string out;
    for (const auto& m : modules_) {
        Metrics mm(m->name());
        try {
            m->metrics(mm);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s.metrics: %s\n", program_.c_str(), m->name().c_str(), e.what());
            continue;
        }
        if (!mm.str().empty()) out += (out.empty() ? "" : " ") + mm.str();
    }
    return out;
}

// ---- 제공 도구: 단계 ---------------------------------------------------------------------------

StageOptions Host::stage_options(const std::string& name, StageOptions defaults) const {
    StageOptions o = defaults;
    if (stages_cfg_.contains(name)) o = parse_stage_options(stages_cfg_[name], name, defaults);
    // 대기 상한은 멈춤 판정의 1/10 이하(정상 대기가 멈춤으로 보이지 않게)
    require(o.wait_ms * 10 <= static_cast<int>(thread_stall_sec_ * 1000),
            "stage " + name + ": wait_ms " + std::to_string(o.wait_ms) + " must be <= thread_stall_sec/10");
    return o;
}

void Host::add_stage(std::unique_ptr<Stage> st) {
    stages_.push_back(std::move(st));
    stage_owner_.push_back(starting_module_);
}

void Host::stop_stages_of(size_t module) {
    for (size_t i = stages_.size(); i-- > 0;)
        if (stage_owner_[i] == module && stages_[i]) stages_[i]->stop();
}

// ---- 기본 안전망 ---------------------------------------------------------------------------------

void Host::note_error(const std::string& what) {
    ++host_errors_;
    if (host_errors_ <= 20 || host_errors_ % 100 == 0)
        std::fprintf(stderr, "%s: error #%llu (caught, continuing): %s\n", program_.c_str(),
                     static_cast<unsigned long long>(host_errors_), what.c_str());
    warnings_.push_back(what);
    if (warnings_.size() > kMaxWarnings) warnings_.erase(warnings_.begin());
}

template <class F>
bool Host::guard(size_t module, const char* where, bool counts_toward_exit, F&& f) {
    const std::string who = modules_[module]->name() + "." + where;
    try {
        if (fault_fatal_ && counts_toward_exit && module == fault_module_) {
            fault_fatal_ = false;
            throw Fatal("injected fatal");
        }
        int& inj = std::string(where) == "on_timer" ? fault_throw_timer_ : fault_throw_poll_;
        if (counts_toward_exit && inj > 0 && module == fault_module_) {
            --inj;
            throw std::runtime_error("injected exception");
        }
        f();
        if (counts_toward_exit) consecutive_errors_[who] = 0;  // 진입점마다 따로 센다
        return true;
    } catch (const Fatal& e) {
        fatal_ = who + ": " + e.what();
    } catch (const std::exception& e) {
        note_error(who + ": " + e.what());
        if (counts_toward_exit && ++consecutive_errors_[who] >= kMaxConsecutiveErrors)
            fatal_ = who + ": " + std::to_string(kMaxConsecutiveErrors) + " exceptions in a row (state may be broken): " + e.what();
    }
    return false;
}

void Host::call_safe_state() {
    if (safe_state_done_) return;
    safe_state_done_ = true;
    for (size_t i = modules_.size(); i-- > 0;) {
        try {
            modules_[i]->safe_state();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s.safe_state: %s\n", program_.c_str(), modules_[i]->name().c_str(), e.what());
        }
    }
}

Json Host::runtime_status() const {
    Json st = Json::object();
    for (const auto& s : stages_)
        if (s) st[s->name()] = s->status();
    return {{"loop", {{"last_ms", static_cast<double>(loop_last_us_) / 1000.0},
                      {"max_ms", static_cast<double>(loop_report_max_us_) / 1000.0},
                      {"budget_ms", loop_budget_ms_},
                      {"budget_overruns", budget_overruns_},
                      {"watchdog_sec", watchdog_sec_},
                      {"thread_stall_sec", thread_stall_sec_}}},
            {"errors", host_errors_},
            {"warnings", warnings_},
            {"stages", st},
            {"fault_injection", fault_enabled_}};
}

std::string Host::host_metrics() {
    loop_report_max_us_ = loop_max_us_;
    char buf[160];
    std::snprintf(buf, sizeof buf, "host_loop_ms=%.2f host_errors=%llu host_budget_overruns=%llu",
                  static_cast<double>(loop_max_us_) / 1000.0, static_cast<unsigned long long>(host_errors_),
                  static_cast<unsigned long long>(budget_overruns_));
    loop_max_us_ = 0;
    std::string out = buf;
    for (const auto& s : stages_)
        if (s) out += " " + s->metrics();
    return out;
}

// 장애 주입(시험): {"kind": "stall", "stage", "ms"} | {"kind": "throw", "where": "timer"|"poll", "count", "module"?}
// | {"kind": "fatal", "module"?}. module 기본은 첫 모듈
bool Host::fault(const Json& args, std::string* error) {
    const std::string kind = args.value("kind", "");
    if (kind == "stall") {
        const std::string name = args.value("stage", "");
        for (auto& st : stages_) {
            if (st && st->name() == name) {
                st->fault_pause(args.value("ms", 1000));
                return true;
            }
        }
        *error = "no stage " + name;
        return false;
    }
    if (kind == "throw" || kind == "fatal") {
        // 대상 모듈(기본: 첫 모듈)
        fault_module_ = 0;
        if (args.contains("module")) {
            const std::string mn = args.value("module", "");
            const auto it = std::find_if(modules_.begin(), modules_.end(), [&](const auto& m) { return m->name() == mn; });
            if (it == modules_.end()) {
                *error = "no module " + mn;
                return false;
            }
            fault_module_ = static_cast<size_t>(it - modules_.begin());
        }
    }
    if (kind == "throw") {
        const int n = args.value("count", 1);
        if (args.value("where", "timer") == "poll") fault_throw_poll_ = n;
        else fault_throw_timer_ = n;
        return true;
    }
    if (kind == "fatal") {
        fault_fatal_ = true;
        return true;
    }
    *error = "kind must be stall, throw or fatal";
    return false;
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
    if (fault_enabled_) std::printf("%s: FAULT INJECTION enabled (test build)\n", program_.c_str());

    // 모듈 시작(붙인 순서). 실패하면 이미 시작한 모듈을 정지하고 끝낸다
    size_t started = 0;
    for (; started < modules_.size(); ++started) {
        starting_module_ = started;
        try {
            modules_[started]->start(*this);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s: %s\n", program_.c_str(), modules_[started]->name().c_str(), e.what());
            stop_stages_of(started);
            while (started-- > 0) {
                stop_stages_of(started);
                modules_[started]->stop();
            }
            return kExitSocket;
        }
    }
    for (const auto& [name, v] : stages_cfg_.items()) {
        (void)v;
        if (std::none_of(stages_.begin(), stages_.end(), [&](const auto& s) { return s->name() == name; }))
            std::fprintf(stderr, "%s: warning: config.stages.%s: no such stage\n", program_.c_str(), name.c_str());
    }

    // 명령하는 곳 허용 목록(process-manager가 띄울 때 WORKSPACE_PEERS_FILE). 없으면 단독 실행: 보지 않음
    if (const char* pf = std::getenv("WORKSPACE_PEERS_FILE"); pf && *pf) set_peers_file(pf);
    CommandServer server([&](int conn, const std::string& line) {
        dispatch(line, [&server, conn](const Json& resp) { server.reply(conn, resp.dump()); }, server.peer_pid(conn));
    });
    if (!server.open(command_socket_, &error)) {
        std::fprintf(stderr, "%s: %s\n", program_.c_str(), error.c_str());
        for (size_t i = modules_.size(); i-- > 0;) {
            stop_stages_of(i);
            modules_[i]->stop();
        }
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
    bool inputs_pending = false;
    std::vector<pollfd> fds;
    const auto stall_exit_after = std::chrono::milliseconds(static_cast<int64_t>(std::max(0.5, watchdog_sec_ - 0.5) * 1000));
    while (!stop_requested()) {
        if (take_reload_request()) {
            // 바로 적용 설정(SIGHUP): 반영하면 config_version이 바뀌고 다음 heartbeat로 Manager가 확인한다
            if (!reload_file(config_path_, &error))
                std::fprintf(stderr, "%s: config not applied live: %s\n", program_.c_str(), error.c_str());
        }
        const auto now = Clock::now();
        // 스레드 감시: 모두 살아 있을 때만 커널 워치독을 다시 맞춘다. 멈춘 스레드가 이어지면 워치독이 끝내기 전에
        // safe_state() 뒤 스스로 끝낸다(exit 4)
        std::string stalled;
        if (watch_->all_alive(now, &stalled)) {
            arm_watchdog(watchdog_sec_);
            stalled_reported.clear();
            stall_since_ = TimePoint{};
        } else {
            if (stalled != stalled_reported) {
                std::fprintf(stderr, "%s: thread stalled: %s (watchdog not refreshed)\n", program_.c_str(), stalled.c_str());
                stalled_reported = stalled;
            }
            if (stall_since_ == TimePoint{}) stall_since_ = now;
            if (now - stall_since_ >= stall_exit_after) fatal_ = "thread stalled: " + stalled;
        }
        std::string why;
        for (size_t i = 0; i < modules_.size() && fatal_.empty(); ++i) {
            bool f = false;
            guard(i, "failed", false, [&] { f = modules_[i]->failed(&why); });
            if (f) fatal_ = modules_[i]->name() + " failed: " + why;
        }
        for (const auto& st : stages_)
            if (fatal_.empty() && st && st->failed(&why)) fatal_ = "stage " + why;
        if (!fatal_.empty()) {
            std::fprintf(stderr, "%s: %s\n", program_.c_str(), fatal_.c_str());
            exit_code = kExitFailed;
            break;
        }

        auto wake = std::min(next_report, now + kMaxSleep);
        for (size_t i = 0; i < modules_.size(); ++i)
            guard(i, "next_wakeup", false, [&] { wake = std::min(wake, modules_[i]->next_wakeup(now)); });
        if (inputs_pending) wake = now;  // 예산을 넘겨 남은 입력이 있으면 바로 다음 차례
        const int timeout_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(wake - now).count());
        fds.clear();
        server.add_pollfds(fds);
        for (size_t i = 0; i < modules_.size(); ++i) guard(i, "add_pollfds", false, [&] { modules_[i]->add_pollfds(fds); });
        for (const auto& st : stages_)
            if (st && st->wake_fd() >= 0) fds.push_back({st->wake_fd(), POLLIN, 0});
        const int n = poll(fds.data(), fds.size(), timeout_ms < 0 ? 0 : timeout_ms);
        if (n < 0 && errno != EINTR) {
            std::fprintf(stderr, "%s: poll: %s\n", program_.c_str(), std::strerror(errno));
            break;
        }
        const auto t = Clock::now();
        if (n > 0) {
            server.on_poll(fds);
            for (size_t i = 0; i < modules_.size(); ++i) guard(i, "on_poll", true, [&] { modules_[i]->on_poll(fds, t); });
        }
        // 입력 단계: 처리 시간 예산 안에서(남으면 다음 차례). 항목 하나의 예외는 단계가 세고 넘어간다
        inputs_pending = false;
        if (!stages_.empty()) {
            const auto deadline = t + std::chrono::milliseconds(loop_budget_ms_);
            for (size_t i = 0; i < stages_.size(); ++i) {
                if (!stages_[i] || stages_[i]->wake_fd() < 0) continue;
                guard(stage_owner_[i], "input", false, [&] { inputs_pending |= stages_[i]->process(deadline); });
            }
            if (inputs_pending) ++budget_overruns_;
        }
        server.on_timer(t);
        sim_tick(t);
        for (size_t i = 0; i < modules_.size(); ++i) guard(i, "on_timer", true, [&] { modules_[i]->on_timer(t); });
        if (t >= next_report) {
            next_report = t + kReportPeriod;
            if (watch_->all_alive(t)) {
                uint64_t progress = 0;
                for (size_t i = 0; i < modules_.size(); ++i) guard(i, "progress", false, [&] { progress += modules_[i]->progress(); });
                std::string mx = metrics();
                const std::string hx = host_metrics();
                mx += (mx.empty() ? "" : " ") + hx;
                notify.send("WATCHDOG=1 id=" + id_ + " progress=" + std::to_string(progress) +
                            " config_version=" + std::to_string(config_version_) + " " + mx);
            }
        }
        // 루프 한 번 시간(기본 안전망): 워치독의 절반을 넘으면 경고(죽기 전에 보이게)
        loop_last_us_ = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t).count();
        loop_max_us_ = std::max(loop_max_us_, loop_last_us_);
        if (loop_last_us_ > static_cast<int64_t>(watchdog_sec_ * 500000) && Clock::now() - last_loop_warn_ > std::chrono::seconds(10)) {
            last_loop_warn_ = Clock::now();
            const std::string w = "slow loop: " + std::to_string(loop_last_us_ / 1000) + " ms (watchdog " +
                                  std::to_string(static_cast<int>(watchdog_sec_)) + " s)";
            std::fprintf(stderr, "%s: warning: %s\n", program_.c_str(), w.c_str());
            warnings_.push_back(w);
            if (warnings_.size() > kMaxWarnings) warnings_.erase(warnings_.begin());
        }
    }

    // 종료 절차 (design.md 6.6): 타이머를 정리 상한으로, 안전 상태, 명령 소켓 닫기, 모듈 정지(반대 순서, 단계 먼저)
    arm_watchdog(stop_timeout_sec_);
    std::printf("%s: stopping%s\n", program_.c_str(), exit_code ? " (failure)" : "");
    call_safe_state();
    server.close();
    for (size_t i = modules_.size(); i-- > 0;) {
        stop_stages_of(i);
        try {
            modules_[i]->stop();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s.stop: %s\n", program_.c_str(), modules_[i]->name().c_str(), e.what());
        }
    }
    if (sim_pid_ > 0) kill_wait(sim_pid_);
    return exit_code;
}

Json call_module(Module& m, const std::string& cmd, const Json& args) {
    auto out = std::make_shared<Json>();  // 늦게 오는 응답이 사라진 지역 변수에 쓰지 않게
    if (!m.handle(cmd, args, [out](const Json& r) { *out = r; })) return error_reply("bad_request", "unknown command");
    return *out;
}

}  // namespace worker
