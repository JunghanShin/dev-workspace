#include "sysmon/module.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "sysmon/tegrastats.hpp"
#include "worker/child.hpp"
#include "worker/runtime.hpp"

namespace sysmon {

namespace {

constexpr double kFirstLineTimeoutS = 5.0;
constexpr int kStaleFactor = 3;  // 수집기 파일이 interval의 이 배수 넘게 안 바뀌면 멈춘 것으로 본다
constexpr size_t kMaxLine = 64 * 1024;

using Clock = worker::Clock;

auto secs(double s) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s)); }
double mono_s(worker::TimePoint t) { return std::chrono::duration<double>(t.time_since_epoch()).count(); }

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

// 로그용 따옴표 글자(Python %r 비슷하게)
std::string quoted(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\'') out += "\\'";
        else out += c;
    }
    return out + "'";
}

long long mtime_ns(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return -1;
    return static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
}

// 수집기 파일이 max_age_s 안에 바뀌었나(벽시계 기준, 옛 판과 같음)
bool fresh(const std::string& path, double max_age_s) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return false;
    timespec now {};
    clock_gettime(CLOCK_REALTIME, &now);
    const double age = static_cast<double>(now.tv_sec - st.st_mtim.tv_sec) + (now.tv_nsec - st.st_mtim.tv_nsec) / 1e9;
    return age <= max_age_s;
}

}  // namespace

std::string which(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (true) {
        const size_t colon = p.find(':', start);
        std::string dir = p.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (dir.empty()) dir = ".";
        const std::string full = dir + "/" + name;
        struct stat st {};
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(full.c_str(), X_OK) == 0) return full;
        if (colon == std::string::npos) return "";
        start = colon + 1;
    }
}

void Module::configure(const worker::Json& config) {
    worker::check_keys(config, {"interval_ms"});
    if (config.contains("interval_ms")) {
        worker::require(config["interval_ms"].is_number() && !config["interval_ms"].is_boolean(), "interval_ms must be int");
        interval_ms_ = static_cast<int>(config["interval_ms"].get<double>());  // Python int()처럼 소수는 버림
    }
    worker::require(interval_ms_ >= 100, "interval_ms must be >= 100");
}

void Module::start(worker::Host& host) {
    watchdog_sec_ = host.watchdog_sec();
    started_ = Clock::now();
    power_mode_ = read_power_mode(root_);
    std::printf("config: interval_ms=%d watchdog_sec=%g\n", interval_ms_, watchdog_sec_);
    // 첫 줄은 tegrastats 시작까지 더 기다린다: 만료 = 첫 줄 상한 + 한 주기 + 여유(호스트 루프가 돌면 다시 맞춘다)
    worker::arm_watchdog(kFirstLineTimeoutS + interval_s() + watchdog_sec_ + 1);

    const char* data = std::getenv("WORKSPACE_DATA");
    const std::string path = data && *data ? std::string(data) + "/run/tegrastats.line" : "";
    if (!path.empty() && fresh(path, interval_s() * kStaleFactor)) {
        collector_ = true;
        collector_path_ = path;
        collector_changed_ = next_check_ = started_;
        std::printf("source: root collector %s\n", path.c_str());
    } else {
        const std::string bin = which("tegrastats");
        if (bin.empty()) {
            fail("tegrastats not found (not a Jetson / JetPack missing?)");
            throw std::runtime_error(fail_);
        }
        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0) throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));
        pid_ = worker::spawn({bin, "--interval", std::to_string(interval_ms_)}, -1, p[1]);
        close(p[1]);
        fd_ = p[0];
        if (pid_ < 0) {
            close_child();
            fail("cannot start " + bin);
            throw std::runtime_error(fail_);
        }
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
        last_line_ = started_;
        std::printf("source: own tegrastats (no root collector; EMC/HW engine values unavailable)\n");
    }

    // 알아볼 수 있는 첫 줄까지(그 뒤 호스트가 READY). SIGTERM이면 그만 기다린다
    const auto deadline = started_ + secs(kFirstLineTimeoutS + interval_s() + watchdog_sec_);
    while (count_ == 0 && fail_.empty() && !worker::stop_requested()) {
        const auto now = Clock::now();
        if (now >= deadline) {
            fail("no tegrastats output for " + fmt("%.1f", kFirstLineTimeoutS + interval_s() + watchdog_sec_) + "s");
            break;
        }
        if (collector_) {
            if (now >= next_check_) check_collector(now);
            std::this_thread::sleep_for(std::min<Clock::duration>(next_check_ - now, std::chrono::milliseconds(100)));
        } else {
            pollfd pfd{fd_, POLLIN, 0};
            if (poll(&pfd, 1, 100) > 0) read_pipe(Clock::now());
        }
    }
    if (!fail_.empty()) {
        close_child();
        throw std::runtime_error(fail_);
    }
}

void Module::fail(const std::string& why) {
    if (fail_.empty()) fail_ = why;
}

void Module::line(const std::string& text, worker::TimePoint now) {
    last_line_ = now;
    MetricList m = parse_tegrastats(text);
    if (m.empty()) {
        if (count_ == 0 && now > started_ + secs(kFirstLineTimeoutS + interval_s()))
            fail("tegrastats output not recognized: " + quoted(text));
        return;
    }
    if (power_mode_) m.set_int("power_mode", *power_mode_);
    m.update(extras_.sample(mono_s(now)));
    m.set_int("collector_root", collector_ ? 1 : 0);
    ++count_;
    latest_ = std::move(m);
    if (count_ == 1) std::printf("first sample: %s\n", latest_.str().c_str());
}

void Module::read_pipe(worker::TimePoint now) {
    if (fd_ < 0) return;
    char chunk[4096];
    bool eof = false;
    while (true) {
        const ssize_t n = read(fd_, chunk, sizeof chunk);
        if (n > 0) {
            buf_.append(chunk, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) eof = true;
        else if (errno == EINTR) continue;
        else if (errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
        break;
    }
    for (size_t nl; (nl = buf_.find('\n')) != std::string::npos;) {
        const std::string l = buf_.substr(0, nl + 1);
        buf_.erase(0, nl + 1);
        line(l, now);
    }
    if (buf_.size() > kMaxLine) {  // 줄바꿈 없이 끝없이 오면 한 줄로 보고 버린다
        line(buf_, now);
        buf_.clear();
    }
    if (!eof) return;
    if (!buf_.empty()) {  // 끝 줄(줄바꿈 없음)
        line(buf_, now);
        buf_.clear();
    }
    close(fd_);
    fd_ = -1;
    // stdout을 닫았으면 곧 끝난다. 0.5 s 안에 안 끝나면 정리(kill_wait)
    int st = 0;
    pid_t r = 0;
    for (int i = 0; i < 25 && (r = waitpid(pid_, &st, WNOHANG)) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::string rc = "?";
    if (r == pid_) {
        rc = WIFEXITED(st) ? std::to_string(WEXITSTATUS(st)) : WIFSIGNALED(st) ? std::to_string(-WTERMSIG(st)) : "?";
        pid_ = -1;
    } else {
        worker::kill_wait(pid_);
    }
    if (!worker::stop_requested()) fail("tegrastats exited (" + rc + ")");
}

void Module::check_collector(worker::TimePoint now) {
    next_check_ = now + secs(interval_s() / 4);
    const long long mt = mtime_ns(collector_path_);
    if (mt >= 0 && mt != collector_mtime_ns_) {
        collector_mtime_ns_ = mt;
        collector_changed_ = now;
        std::ifstream in(collector_path_, std::ios::binary);
        std::stringstream ss;
        if (in) ss << in.rdbuf();
        const std::string text = ss.str();
        if (!text.empty()) line(text, now);
    } else if (now - collector_changed_ > secs(interval_s() * kStaleFactor)) {
        fail("collector " + collector_path_ + " not updated for " +
             fmt("%.1f", std::chrono::duration<double>(now - collector_changed_).count()) +
             "s; exiting so the Manager restarts with own tegrastats");
    }
}

void Module::check_silence(worker::TimePoint now) {
    // 옛 판은 자체 워치독(watchdog_sec + interval)으로 끝났다. 호스트 루프는 막히지 않으므로 여기서 본다
    const double limit = watchdog_sec_ + interval_s();
    if (now - last_line_ > secs(limit))
        fail("tegrastats silent for " + fmt("%.1f", std::chrono::duration<double>(now - last_line_).count()) + "s");
}

void Module::add_pollfds(std::vector<pollfd>& fds) {
    if (fd_ >= 0) fds.push_back({fd_, POLLIN, 0});
}

void Module::on_poll(const std::vector<pollfd>& fds, worker::TimePoint now) {
    for (const auto& p : fds)
        if (p.fd == fd_ && fd_ >= 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) read_pipe(now);
}

void Module::on_timer(worker::TimePoint now) {
    if (!fail_.empty()) return;
    if (collector_) {
        if (now >= next_check_) check_collector(now);
    } else if (pid_ > 0) {
        check_silence(now);
    }
}

worker::TimePoint Module::next_wakeup(worker::TimePoint now) const {
    const auto later = now + std::chrono::seconds(1);
    if (collector_) return std::min(later, next_check_);
    return std::min(later, last_line_ + secs(watchdog_sec_ + interval_s()));
}

worker::Json Module::status() const {
    worker::Json m = worker::Json::object();
    for (const auto& x : latest_.items()) {
        if (x.text.find_first_of(".en") != std::string::npos) m[x.key] = x.value;
        else m[x.key] = static_cast<long long>(x.value);
    }
    return {{"source", collector_ ? "root collector" : "own tegrastats"},
            {"collector_root", collector_ ? 1 : 0},
            {"collector", collector_ ? worker::Json(collector_path_) : worker::Json(nullptr)},
            {"interval_ms", interval_ms_},
            {"lines", count_},
            {"power_mode", power_mode_ ? worker::Json(*power_mode_) : worker::Json(nullptr)},
            {"metrics", m}};
}

void Module::metrics(worker::Metrics& m) const {
    m.raw(latest_.str());  // 옛 이름 그대로(접두어 "sysmon_" 없이). 화면·기록이 이 이름을 쓴다
}

bool Module::failed(std::string* why) const {
    if (fail_.empty()) return false;
    if (why) *why = fail_;
    return true;
}

void Module::close_child() {
    if (pid_ > 0) worker::kill_wait(pid_, std::chrono::seconds(2));  // 옛 판: terminate → 2 s → kill
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
}

void Module::stop() { close_child(); }

}  // namespace sysmon
