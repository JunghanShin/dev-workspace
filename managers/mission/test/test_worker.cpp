// mission-manager 통합 시험: 실제 실행 파일과 가짜 워커 명령 소켓(느린 응답)으로
//   1) 같은 역할 동시 명령은 차례로(겹치지 않음), 다른 역할은 함께
//   2) 대기가 차면 busy
//   3) 워커가 받는 봉투에 origin(보낸 곳), 응답은 그대로 돌아옴
//   4) 잘못된 설정 → exit 1
//
//   test_worker [에뮬레이터...] <mission-manager 경로>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
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
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(fd, 16) != 0) return -1;
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

static std::string read_line(int fd, int ms) {
    std::string out;
    char c;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 50) <= 0) continue;
        if (read(fd, &c, 1) != 1) break;
        if (c == '\n') return out;
        out += c;
    }
    return out;
}

static void send_line(int fd, const std::string& s) {
    const std::string l = s + "\n";
    (void)!write(fd, l.data(), l.size());
}

// 가짜 워커 명령 소켓: 요청 하나 = 연결 하나. "slow.*"는 400 ms 뒤 답. 역할(앞부분)마다 동시에 처리 중인 수의 최대를 잰다
struct FakeWorker {
    int lfd = -1;
    std::atomic<bool> stop{false};
    std::thread th;
    std::mutex mu;
    std::map<std::string, int> busy, peak;
    std::vector<Json> got;
    std::vector<std::thread> handlers;

    void start(const std::string& path) {
        lfd = listen_unix(path);
        th = std::thread([this] {
            while (!stop) {
                pollfd p{lfd, POLLIN, 0};
                if (poll(&p, 1, 50) <= 0) continue;
                const int c = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
                if (c < 0) continue;
                std::lock_guard<std::mutex> lock(mu);
                handlers.emplace_back([this, c] { serve(c); });
            }
        });
    }
    void serve(int c) {
        const Json req = Json::parse(read_line(c, 5000), nullptr, false);
        const std::string cmd = req.is_object() ? req.value("cmd", "") : "";
        const std::string role = cmd.substr(0, cmd.find('.'));
        {
            std::lock_guard<std::mutex> lock(mu);
            got.push_back(req);
            peak[role] = std::max(peak[role], ++busy[role]);
        }
        if (cmd.rfind("slow.", 0) == 0) std::this_thread::sleep_for(400ms);
        {
            std::lock_guard<std::mutex> lock(mu);
            --busy[role];
        }
        send_line(c, Json{{"ok", true}, {"data", {{"cmd", cmd}}}}.dump());
        close(c);
    }
    void finish() {
        stop = true;
        if (th.joinable()) th.join();
        for (auto& h : handlers) h.join();
        close(lfd);
    }
};

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
        unsetenv("WORKSPACE_PEERS_FILE");
        for (const auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        execvp(argv[0], argv.data());
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

static Json mission_send(const std::string& sock, const std::string& cmd) {
    const int fd = connect_unix(sock);
    if (fd < 0) return Json();
    send_line(fd, Json{{"cmd", "mission.send"}, {"args", {{"target", "w1"}, {"cmd", cmd}, {"args", {{"n", 1}}}}}, {"from", "screen"}}.dump());
    const Json r = Json::parse(read_line(fd, 8000), nullptr, false);
    close(fd);
    return r;
}

static void test_mission() {
    const std::string data = g_dir + "/data";
    (void)!std::system(("mkdir -p " + data + "/run/cmd " + data + "/run/stream").c_str());
    FakeWorker w;
    w.start(data + "/run/cmd/w1.sock");
    const pid_t pid = spawn(R"({"id":"mission-manager","config_version":1,"config":{"mission":{"queue":4}}})",
                            {{"WORKSPACE_DATA", data}, {"WORKSPACE_MANAGER_SOCKET", g_dir + "/no-pm.sock"}});
    const std::string sock = data + "/run/cmd/mission-manager.sock";
    for (int i = 0; i < 150 && connect_unix(sock) < 0; ++i) std::this_thread::sleep_for(100ms);

    // 1) slow 둘(같은 역할) + fast 하나(다른 역할)를 동시에
    std::vector<Json> res(3);
    std::vector<double> took(3);
    std::vector<std::thread> ts;
    const auto t0 = std::chrono::steady_clock::now();
    const char* cmds[] = {"slow.a", "slow.b", "fast.c"};
    for (int i = 0; i < 3; ++i)
        ts.emplace_back([&, i] {
            if (i == 2) std::this_thread::sleep_for(50ms);
            res[i] = mission_send(sock, cmds[i]);
            took[i] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        });
    for (auto& t : ts) t.join();
    for (int i = 0; i < 3; ++i) CHECK(res[i].is_object() && res[i].value("ok", false) && res[i]["data"]["cmd"] == cmds[i]);
    CHECK(took[2] < 0.35);                         // 다른 역할은 기다리지 않음
    CHECK(std::max(took[0], took[1]) > 0.75);      // 같은 역할은 차례로(400 ms × 2)
    {
        std::lock_guard<std::mutex> lock(w.mu);
        CHECK(w.peak["slow"] == 1);                // 겹치지 않음
        CHECK(!w.got.empty() && w.got[0].value("origin", "") == "screen");  // 처음 보낸 곳이 실려 감
    }
    // 2) slow 7개 동시: 보낸 1 + 대기 4 = 5개 성공, 2개 busy
    std::vector<Json> many(7);
    ts.clear();
    for (int i = 0; i < 7; ++i) ts.emplace_back([&, i] { many[i] = mission_send(sock, "slow.q"); });
    for (auto& t : ts) t.join();
    int ok = 0, busy = 0;
    for (const auto& r : many) {
        if (r.value("ok", false)) ++ok;
        else if (r.contains("error") && r["error"].value("code", "") == "busy") ++busy;
    }
    CHECK(ok == 5 && busy == 2);
    // 큐 보기(읽기)
    const int fd = connect_unix(sock);
    send_line(fd, R"({"cmd":"mission.get_queues","args":{}})");
    const Json q = Json::parse(read_line(fd, 5000), nullptr, false);
    close(fd);
    CHECK(q.value("ok", false) && q["data"]["sent"].get<int>() >= 8 && q["data"]["busy"] == 2);
    kill(pid, SIGTERM);
    CHECK(wait_exit(pid, 15000) == 0);
    w.finish();
}

static void test_bad_config() {
    CHECK(wait_exit(spawn(R"({"id":"mission-manager","config":{"mission":{"nope":1}}})", {}), 15000) == 1);
    CHECK(wait_exit(spawn(R"({"id":"mission-manager","config":{"mission":{"command_timeout_ms":3000,"total_timeout_ms":1000}}})", {}), 15000) == 1);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_worker [emulator...] <mission-manager>\n");
        return 2;
    }
    for (int i = 1; i < argc; ++i) g_cmd.push_back(argv[i]);
    char tmpl[] = "/tmp/mi-it-XXXXXX";
    g_dir = mkdtemp(tmpl);
    signal(SIGPIPE, SIG_IGN);
    test_mission();
    test_bad_config();
    (void)!std::system(("rm -rf " + g_dir).c_str());
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("mission-manager: all tests passed\n");
    return 0;
}
