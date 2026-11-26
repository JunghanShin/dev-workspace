#include "productbuild.hpp"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <ctime>

#include "log.hpp"
#include "proc.hpp"
#include "sys.hpp"
#include "ui_rules.hpp"

namespace gw {

namespace {

constexpr double kChildTimeoutS = 60;   // --profiles·--check 상한
constexpr double kProfilesCacheS = 30;  // 화면은 진행 중 1초마다 상태를 읽으므로 묶어 둔다

void mkdir_p(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || (path[i] == '/' && i > 0)) {
            cur = path.substr(0, i);
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                throw std::runtime_error("mkdir " + cur + ": " + errno_text(errno));
        }
    }
}

}  // namespace

ProductBuild::ProductBuild(std::string script, std::string data_dir, std::string python)
    : script_(std::move(script)), out_(path_join(data_dir.empty() ? "/tmp" : data_dir, "dist")), python_(std::move(python)) {}

ProductBuild::~ProductBuild() { shutdown(); }

void ProductBuild::shutdown() {
    stop_.store(true);
    if (reader_.joinable()) reader_.join();
}

std::vector<std::string> ProductBuild::env_extra() const {
    // 자식 Python의 입출력을 UTF-8로(서비스 환경은 LANG이 없어 한글 진행 글이 깨진다)
    return {"PYTHONIOENCODING=utf-8", "PYTHONUTF8=1"};
}

Json ProductBuild::profiles() {
    const double now = wall_now();
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (now - profiles_t_ <= kProfilesCacheS) return profiles_;
    }
    const RunResult r = run_capture({python_, script_, "--profiles"}, kChildTimeoutS, env_extra());
    const std::string out = utf8_replace(r.out);
    auto j = (r.started && !r.timed_out) ? parse_json(out.empty() ? "[]" : out) : std::nullopt;
    if (!j) {
        GW_ERROR("profiles: %s", !r.started ? errno_text(r.spawn_err).c_str() : r.timed_out ? "timed out" : "bad JSON");
        return Json::array();
    }
    std::lock_guard<std::mutex> lock(mu_);
    profiles_t_ = now;
    profiles_ = *j;
    return profiles_;
}

Json ProductBuild::check(const std::string& profile) {
    const RunResult r = run_capture({python_, script_, profile, "--check", "--json", "--local", "--out", out_},
                                    kChildTimeoutS, env_extra());
    const std::string out = utf8_replace(r.out);
    if (r.started && !r.timed_out) {
        auto lines = splitlines(strip(out));
        if (!lines.empty())
            if (auto j = parse_json(lines.back())) return *j;
    }
    auto msg = splitlines(strip(out + utf8_replace(r.err)));
    std::string detail = msg.empty() ? "failed" : msg.back();
    if (!r.started) detail = errno_text(r.spawn_err);
    else if (r.timed_out) detail = "timed out after 60 seconds";
    Json item = Json::object();
    item["key"] = "error";
    item["label"] = "점검";
    item["ok"] = false;
    item["detail"] = detail;
    Json o = Json::object();
    o["ok"] = false;
    o["profile"] = profile;
    o["items"] = Json::array({item});
    return o;
}

Json ProductBuild::last() const {
    struct F {
        std::string path;
        double mtime;
    };
    std::vector<F> files;
    for (const auto& n : list_dir(out_))
        if (!starts_with(n, ".") && ends_with(n, ".tar.gz")) {
            const std::string p = path_join(out_, n);
            files.push_back({p, mtime_of(p)});
        }
    if (files.empty()) return nullptr;
    std::stable_sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.mtime < b.mtime; });
    const F& f = files.back();
    const time_t t = static_cast<time_t>(f.mtime);
    tm lt{};
    localtime_r(&t, &lt);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &lt);
    Json o = Json::object();
    o["file"] = basename(f.path);
    o["path"] = f.path;
    o["mb"] = py_round(static_cast<double>(size_of(f.path)) / 1e6, 1);
    o["created"] = buf;
    return o;
}

Json ProductBuild::status() {
    Json s;
    {
        std::lock_guard<std::mutex> lock(mu_);
        s = st_.st;
        Json log = Json::array();
        const size_t from = st_.log.size() > 80 ? st_.log.size() - 80 : 0;
        for (size_t i = from; i < st_.log.size(); ++i) log.push_back(st_.log[i]);
        s["log"] = log;
    }
    if (s.contains("started") && py_truthy(s["started"])) {
        const double end = (s.contains("finished") && py_truthy(s["finished"])) ? s["finished"].get<double>() : wall_now();
        s["elapsed"] = py_round(end - s["started"].get<double>(), 1);
    }
    s["last"] = last();
    s["profiles"] = profiles();
    return s;
}

Json ProductBuild::start(const std::string& profile, const std::string& power_mode, const std::string& login) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (st_.st.value("state", "") == "running")
            throw Busy("a build is already running (" + py_str(st_.st.value("profile", Json(nullptr))) + ")");
        // 취소한 빌드의 자식이 아직 끝나는 중이면 기다리지 않고 바쁨으로(옛 출력이 새 상태에 섞이지 않게)
        if (reading_.load()) throw Busy("the previous build is still stopping");
    }
    if (reader_.joinable()) reader_.join();  // 이미 끝난 스레드(reading_ == false)
    mkdir_p(out_);
    std::vector<std::string> cmd = {python_, "-u", script_, profile, "--local", "--replace", "--out", out_};
    if (!power_mode.empty()) {
        cmd.push_back("--power-mode");
        cmd.push_back(power_mode);
    }
    if (!login.empty()) {
        cmd.push_back("--login");
        cmd.push_back(login);
    }
    // 새 세션: 취소할 때 Nuitka·gcc 자식까지 한 번에(프로세스 그룹)
    const Child c = spawn_stream(cmd, env_extra(), true);
    if (c.pid < 0) throw std::runtime_error("start build: " + errno_text(c.err));
    {
        std::lock_guard<std::mutex> lock(mu_);
        st_.log.clear();
        Json st = Json::object();
        st["state"] = "running";
        st["profile"] = profile;
        st["power_mode"] = power_mode.empty() ? Json(nullptr) : Json(power_mode);
        st["login"] = login.empty() ? Json(nullptr) : Json(login);
        st["pct"] = 0;
        st["step"] = "시작";
        st["started"] = wall_now();
        st["finished"] = nullptr;
        st["error"] = nullptr;
        st["result"] = nullptr;
        st_.st = st;
        pid_ = c.pid;
    }
    reading_.store(true);
    reader_ = std::thread([this, c] { read_output(c.pid, c.fd); });
    GW_INFO("product build started: %s", profile.c_str());
    return status();
}

void ProductBuild::read_output(pid_t pid, int fd) {
    pthread_setname_np(pthread_self(), "gw.build");
    std::string cur;
    bool cr = false;
    bool open = true;
    while (open && !stop_.load()) {
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, 1000);  // 1 s마다 끝내기 요청을 본다
        if (r == 0) continue;
        if (r < 0 && errno == EINTR) continue;
        char buf[4096];
        const ssize_t n = r < 0 ? -1 : ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        std::vector<std::string> lines;
        for (ssize_t i = 0; i < n; ++i) {  // 텍스트 모드: \r\n, \r, \n 모두 줄 끝
            const char ch = buf[i];
            if (cr && ch == '\n') {
                cr = false;
                continue;
            }
            cr = ch == '\r';
            if (ch == '\n' || ch == '\r') {
                lines.push_back(utf8_replace(cur));
                cur.clear();
            } else {
                cur += ch;
            }
        }
        if (!lines.empty()) {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& l : lines) st_.apply_line(l);
        }
    }
    if (!cur.empty() && !stop_.load()) {
        std::lock_guard<std::mutex> lock(mu_);
        st_.apply_line(utf8_replace(cur));
    }
    ::close(fd);
    int code = -1;
    while (!stop_.load() && !try_reap(pid, code)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::string state, error;
    {
        std::lock_guard<std::mutex> lock(mu_);
        st_.finish(code, wall_now());
        pid_ = -1;
        state = st_.st.value("state", "");
        error = py_truthy(st_.st.value("error", Json(nullptr))) ? py_str(st_.st["error"]) : "";
    }
    GW_INFO("product build %s (%s)", state.c_str(), error.c_str());
    reading_.store(false);
}

bool ProductBuild::cancel() {
    std::lock_guard<std::mutex> lock(mu_);
    if (st_.st.value("state", "") != "running" || pid_ < 0) return false;
    st_.st["state"] = "cancelled";
    st_.st["error"] = "취소됨";
    ::killpg(pid_, SIGTERM);  // 신호 보내기는 막히지 않는다
    return true;
}

}  // namespace gw
