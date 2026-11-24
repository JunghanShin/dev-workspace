// config-manager 통합 시험: 실제 실행 파일을 가짜 process-manager와 임시 워크스페이스·데이터 폴더에 붙인다.
//   1) config.versions: BUILD 읽기(맞음·어긋남)
//   2) 시작할 때 자동 스냅숏 하나(auto), 바뀐 것이 없으면 다시 남기지 않음
//   3) 워커 설정이 바뀜 → config.diff에 그 칸
//   4) config.snapshot(이름) → config.restore(처음 것) → process-manager에 set_config(되돌린 값)
//   5) 잘못된 설정 → exit 1
//
//   test_worker [에뮬레이터...] <config-manager 경로>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

// 가짜 process-manager: 워커 cam1 하나. 설정은 fps(시험이 바꿈). 쓰기 요청은 기록
struct FakePm {
    int lfd = -1;
    std::atomic<bool> stop{false};
    std::atomic<int> fps{30};
    std::thread th;
    std::mutex mu;
    std::vector<Json> writes;
    std::vector<std::thread> conns;
    void start(const std::string& path) {
        lfd = listen_unix(path);
        th = std::thread([this] {
            while (!stop) {
                pollfd p{lfd, POLLIN, 0};
                if (poll(&p, 1, 50) <= 0) continue;
                const int c = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
                if (c < 0) continue;
                std::lock_guard<std::mutex> lock(mu);
                conns.emplace_back([this, c] { serve(c); });
            }
        });
    }
    void serve(int c) {
        const Json req = Json::parse(read_line(c, 5000), nullptr, false);
        const std::string op = req.is_object() ? req.value("op", "") : "";
        if (op == "subscribe") {
            send_line(c, R"({"ok":true})");
            while (!stop) {
                send_line(c, R"({"event":"worker_status","data":{"id":"cam1","libworker":"0.14.0","autostart":true}})");
                for (int i = 0; i < 10 && !stop; ++i) std::this_thread::sleep_for(50ms);
            }
        } else if (op == "list") {
            send_line(c, R"({"ok":true,"data":[{"id":"cam1","autostart":true}]})");
        } else if (op == "get_config") {
            send_line(c, Json{{"ok", true}, {"data", {{"preset", "default"}, {"config", {{"fps", fps.load()}}}}}}.dump());
        } else if (op == "get_connections") {
            send_line(c, R"({"ok":true,"data":{"connections":[],"commanders":{}}})");
        } else {
            {
                std::lock_guard<std::mutex> lock(mu);
                writes.push_back(req);
            }
            send_line(c, R"({"ok":true,"data":{}})");
        }
        close(c);
    }
    void finish() {
        stop = true;
        if (th.joinable()) th.join();
        for (auto& t : conns) t.join();
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

static Json cmd(const std::string& sock, const std::string& name, const Json& args = Json::object()) {
    const int fd = connect_unix(sock);
    if (fd < 0) return Json::object();
    send_line(fd, Json{{"cmd", name}, {"args", args}}.dump());
    const Json r = Json::parse(read_line(fd, 8000), nullptr, false);
    close(fd);
    return r.is_object() ? r : Json::object();
}

static void test_config() {
    const std::string root = g_dir + "/ws", data = g_dir + "/data", pmsock = g_dir + "/pm.sock";
    (void)!std::system(("mkdir -p " + root + "/manager/bin " + root + "/managers/rec/bin " + data + "/run/cmd " + data + "/run/stream " +
                        data + "/config").c_str());
    std::ofstream(root + "/manager/bin/process-manager") << "binary";
    std::ofstream(root + "/manager/bin/BUILD")
        << "source_commit: abc\nbuilt: t\nsha256:\n  ae56f2d5bdc3e6c7e8d6a2a4a1f3f6b4c5e1f7a8b9c0d1e2f3a4b5c6d7e8f9a0  process-manager\n";
    std::ofstream(root + "/managers/rec/bin/record-manager") << "rec";
    std::ofstream(root + "/managers/rec/bin/BUILD")
        << "source_commit: def\nsha256:\n  " "fdd9ad5b0d1f31a0b9ab5e9a4b0f1f1f4f9bd5c6bfe5a9f8b9b3f0a9c1d2e3f4" "  record-manager\n";
    std::ofstream(data + "/config/workspace.env") << "WORKSPACE_REST_PORT=8080\n";
    FakePm pm;
    pm.start(pmsock);
    const pid_t pid = spawn(R"({"id":"config-manager","config_version":1,"config":{"config":{"keep":5}}})",
                            {{"WORKSPACE_DATA", data}, {"WORKSPACE_ROOT", root}, {"WORKSPACE_MANAGER_SOCKET", pmsock}});
    const std::string sock = data + "/run/cmd/config-manager.sock";
    Json v;
    for (int i = 0; i < 150; ++i) {
        v = cmd(sock, "config.versions");
        if (v.value("ok", false) && v["data"]["versions"].contains("manager")) break;
        std::this_thread::sleep_for(100ms);
    }
    CHECK(v.value("ok", false) && v["data"]["versions"].contains("manager") && v["data"]["versions"].contains("managers/rec"));
    CHECK(v.value("ok", false) && v["data"]["versions"]["manager"]["commit"] == "abc");
    CHECK(v.value("ok", false) && v["data"]["mismatch"].size() == 2);  // 두 BUILD의 sha가 실제 파일과 다름
    // 자동 스냅숏 하나
    Json s;
    for (int i = 0; i < 50; ++i) {
        s = cmd(sock, "config.snapshots");
        if (s.value("ok", false) && !s["data"]["snapshots"].empty()) break;
        std::this_thread::sleep_for(100ms);
    }
    CHECK(s.value("ok", false) && s["data"]["snapshots"].size() == 1 && s["data"]["snapshots"][0]["label"] == "auto");
    const std::string first = s.value("ok", false) && !s["data"]["snapshots"].empty() ? s["data"]["snapshots"][0]["id"].get<std::string>() : "";
    // 설정이 바뀜 → diff
    pm.fps = 15;
    Json d = cmd(sock, "config.diff", {{"id", first}});
    bool fps_changed = false;
    for (const auto& c : d.value("data", Json::object()).value("changes", Json::array()))
        fps_changed |= c["path"] == "workers.cam1.config.fps" && c["from"] == 30 && c["to"] == 15;
    CHECK(fps_changed);
    CHECK(cmd(sock, "config.diff", {{"id", "nope"}}).value("error", Json::object()).value("code", "") == "not_found");
    Json r = cmd(sock, "config.snapshot", {{"label", "mine"}});
    CHECK(r.value("ok", false) && r["data"]["saved"] == true);
    // 되돌리기 → set_config fps 30
    r = cmd(sock, "config.restore", {{"id", first}});
    CHECK(r.value("ok", false) && r["data"]["results"]["cam1"]["changed"] == true);
    {
        std::lock_guard<std::mutex> lock(pm.mu);
        bool set = false, conn = false;
        for (const auto& w : pm.writes) {
            set |= w.value("op", "") == "set_config" && w["id"] == "cam1" && w["config"]["fps"] == 30;
            conn |= w.value("op", "") == "set_connections";
        }
        CHECK(set && conn);
    }
    kill(pid, SIGTERM);
    CHECK(wait_exit(pid, 15000) == 0);
    pm.finish();
}

static void test_bad_config() {
    CHECK(wait_exit(spawn(R"({"id":"config-manager","config":{"config":{"nope":1}}})", {}), 15000) == 1);
    CHECK(wait_exit(spawn(R"({"id":"config-manager","config":{"config":{"keep":0}}})", {}), 15000) == 1);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_worker [emulator...] <config-manager>\n");
        return 2;
    }
    for (int i = 1; i < argc; ++i) g_cmd.push_back(argv[i]);
    char tmpl[] = "/tmp/cf-it-XXXXXX";
    g_dir = mkdtemp(tmpl);
    signal(SIGPIPE, SIG_IGN);
    test_config();
    test_bad_config();
    (void)!std::system(("rm -rf " + g_dir).c_str());
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("config-manager: all tests passed\n");
    return 0;
}
