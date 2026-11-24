// status-manager 통합 시험: 실제 실행 파일을 가짜 process-manager 소켓에 붙이고, 내보내는 주제 system을 구독한다.
//   1) 워커 running → system ok(+ 이벤트 health_changed), 링크 지표 0 → degraded(+ worker_health_changed)
//   2) 명령 status.get_system(읽기)
//   3) process-manager가 끊김 → pm_link false, degraded
//   4) 잘못된 설정 → exit 1
//
//   test_worker [에뮬레이터...] <status-manager 경로>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

using Json = nlohmann::json;
using namespace std::chrono_literals;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static std::vector<std::string> g_cmd;
static std::string g_dir;

static int listen_unix(const std::string& path) {
    ::unlink(path.c_str());
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(fd, 4) != 0) return -1;
    return fd;
}

static int connect_unix(const std::string& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int accept_for(int lfd, int ms) {
    pollfd p{lfd, POLLIN, 0};
    if (poll(&p, 1, ms) <= 0) return -1;
    return accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
}

static std::string read_line(int fd, int ms, std::string& buf) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        const size_t pos = buf.find('\n');
        if (pos != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            return line;
        }
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 50) <= 0) continue;
        char tmp[65536];
        const ssize_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
    }
    return "";
}

static void send_line(int fd, const std::string& s) {
    const std::string l = s + "\n";
    (void)!write(fd, l.data(), l.size());
}

static pid_t spawn(const std::string& config, const std::vector<std::pair<std::string, std::string>>& env) {
    const std::string path = g_dir + "/config.json";
    std::ofstream(path) << config;
    const pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> argv;
        for (auto& s : g_cmd) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(const_cast<char*>("--config"));
        argv.push_back(const_cast<char*>(path.c_str()));
        argv.push_back(nullptr);
        unsetenv("NOTIFY_SOCKET");
        unsetenv("WORKSPACE_INPUTS_FILE");
        for (const auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        execvp(argv[0], argv.data());  // 에뮬레이터는 이름만 올 수 있다
        _exit(127);
    }
    return pid;
}

static int wait_exit(pid_t pid, int ms) {
    for (int i = 0; i < ms / 20; ++i) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -2;
        std::this_thread::sleep_for(20ms);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return -1;
}

// 주제 system을 조건이 맞을 때까지 읽는다. 받은 이벤트 code는 events에 모은다
static Json wait_system(int fd, std::string& buf, const std::function<bool(const Json&)>& ok, std::vector<std::string>& events,
                        int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        const std::string line = read_line(fd, 500, buf);
        if (line.empty()) continue;
        const Json j = Json::parse(line, nullptr, false);
        if (!j.is_object()) continue;
        for (const auto& e : j.value("events", Json::array())) events.push_back(e.value("code", ""));
        if (j.contains("topics") && j["topics"].contains("system") && ok(j["topics"]["system"])) return j["topics"]["system"];
    }
    return Json();
}

static std::string worker_line(const std::string& id, int link) {
    return R"({"event":"worker_status","data":{"id":")" + id + R"(","actual_state":"running","desired_state":"running",)"
           R"("autostart":true,"kind":"worker","last_heartbeat_ms_ago":50,"metrics":{"link_link":)" + std::to_string(link) + "}}}";
}

static void test_system() {
    const std::string data = g_dir + "/data", pm = g_dir + "/pm.sock";
    (void)!std::system(("mkdir -p " + data + "/run/stream " + data + "/run/cmd").c_str());
    const int pfd = listen_unix(pm);
    const pid_t pid = spawn(R"({"id":"status-manager","config_version":1,"config":{"status":{"publish_ms":300}}})",
                            {{"WORKSPACE_DATA", data}, {"WORKSPACE_MANAGER_SOCKET", pm}});
    const int pc = accept_for(pfd, 15000);
    CHECK(pc >= 0);
    std::string pbuf;
    const Json sub = Json::parse(read_line(pc, 10000, pbuf), nullptr, false);
    CHECK(sub.is_object() && sub.value("op", "") == "subscribe");
    send_line(pc, R"({"ok":true})");
    send_line(pc, worker_line("a", 1));
    // 주제 구독(스트림 소켓이 생길 때까지 기다림)
    int sc = -1;
    for (int i = 0; i < 150 && sc < 0; ++i) {
        sc = connect_unix(data + "/run/stream/status-manager.sock");
        if (sc < 0) std::this_thread::sleep_for(100ms);
    }
    CHECK(sc >= 0);
    std::string sbuf;
    std::vector<std::string> events;
    Json s = wait_system(sc, sbuf, [](const Json& j) { return j.value("health", "") == "ok"; }, events, 10000);
    CHECK(s.value("health", "") == "ok");
    CHECK(s.value("pm_link", false) == true);
    CHECK(s.contains("clock") && s["clock"].contains("t_unix") && s["clock"].contains("boot_id"));
    CHECK(s.contains("workers") && s["workers"].contains("a") && s["workers"]["a"]["required"] == true);
    // 링크 0 → degraded, 이벤트 둘
    send_line(pc, worker_line("a", 0));
    s = wait_system(sc, sbuf, [](const Json& j) { return j.value("health", "") == "degraded"; }, events, 10000);
    CHECK(s.value("health", "") == "degraded");
    CHECK(s["workers"]["a"]["why"] == "link down (link_link=0)");
    bool worker_ev = false, sys_ev = false;
    for (const auto& c : events) {
        worker_ev |= c == "worker_health_changed";
        sys_ev |= c == "health_changed";
    }
    CHECK(worker_ev && sys_ev);
    // 명령 get_system
    const int cc = connect_unix(data + "/run/cmd/status-manager.sock");
    CHECK(cc >= 0);
    send_line(cc, R"({"cmd":"status.get_system","args":{}})");
    std::string cbuf;
    const Json rep = Json::parse(read_line(cc, 5000, cbuf), nullptr, false);
    CHECK(rep.is_object() && rep.value("ok", false) && rep["data"]["health"] == "degraded");
    close(cc);
    // process-manager 끊김 → pm_link false
    close(pc);
    s = wait_system(sc, sbuf, [](const Json& j) { return j.value("pm_link", true) == false; }, events, 10000);
    CHECK(s.is_object() && s.value("pm_link", true) == false && s.value("health", "") == "degraded");
    kill(pid, SIGTERM);
    CHECK(wait_exit(pid, 15000) == 0);
    close(sc);
    close(pfd);
}

static void test_bad_config() {
    CHECK(wait_exit(spawn(R"({"id":"status-manager","config":{"status":{"nope":1}}})", {}), 15000) == 1);
    CHECK(wait_exit(spawn(R"({"id":"status-manager","config":{"status":{"publish_ms":5}}})", {}), 15000) == 1);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_worker [emulator...] <status-manager>\n");
        return 2;
    }
    for (int i = 1; i < argc; ++i) g_cmd.push_back(argv[i]);
    char tmpl[] = "/tmp/st-it-XXXXXX";
    g_dir = mkdtemp(tmpl);
    signal(SIGPIPE, SIG_IGN);
    test_system();
    test_bad_config();
    (void)!std::system(("rm -rf " + g_dir).c_str());
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("status-manager: all tests passed\n");
    return 0;
}
