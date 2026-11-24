// record-manager 통합 시험: 실제 실행 파일을 가짜 process-manager 소켓에 붙여 기록 파일을 본다.
//   1) start → connected → worker(바뀜·간격)·command·system → SIGTERM → stop, exit 0. 줄 모양은 예전 logger와 같음
//   2) process-manager가 끊김 → disconnected 한 줄, 다시 붙으면 connected
//   3) 잘못된 설정·모르는 키 → exit 1
//
//   test_worker [에뮬레이터...] <record-manager 경로>
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
#include <dirent.h>
#include <fstream>
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

static int accept_for(int lfd, int ms) {
    pollfd p{lfd, POLLIN, 0};
    if (poll(&p, 1, ms) <= 0) return -1;
    return accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
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

static pid_t spawn(const std::string& config) {
    const std::string path = g_dir + "/config.json";
    std::ofstream(path) << config;
    const pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> argv;
        for (auto& s : g_cmd) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(const_cast<char*>("--config"));
        argv.push_back(const_cast<char*>(path.c_str()));
        argv.push_back(nullptr);
        unsetenv("WORKSPACE_DATA");
        unsetenv("NOTIFY_SOCKET");
        execv(argv[0], argv.data());
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

static std::vector<Json> records(const std::string& dir) {
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 6 && n.substr(n.size() - 6) == ".jsonl") names.push_back(n);
        }
        closedir(d);
    }
    std::vector<Json> out;
    for (const auto& n : names) {
        std::ifstream f(dir + "/" + n);
        std::string line;
        while (std::getline(f, line)) out.push_back(Json::parse(line, nullptr, false));
    }
    return out;
}

static int count(const std::vector<Json>& rs, const std::string& type, const std::string& event = "") {
    int n = 0;
    for (const auto& r : rs)
        if (r.value("type", "") == type && (event.empty() || r.value("event", "") == event)) ++n;
    return n;
}

static std::string config(const std::string& rec_dir, const std::string& sock, const std::string& extra = "") {
    return R"({"id":"record-manager","config_version":1,"config":{"record":{"dir":")" + rec_dir + R"(","manager_socket":")" +
           sock + R"(","interval_s":0.3)" + extra + "}}}";
}

static void test_basic() {
    const std::string sock = g_dir + "/pm.sock", rec = g_dir + "/rec1";
    const int lfd = listen_unix(sock);
    CHECK(lfd >= 0);
    const pid_t pid = spawn(config(rec, sock));
    const int c = accept_for(lfd, 5000);
    CHECK(c >= 0);
    const Json sub = Json::parse(read_line(c, 2000), nullptr, false);
    CHECK(sub.value("op", "") == "subscribe" && sub["topics"].size() == 3 && sub.value("period_ms", 0) == 300);
    send_line(c, R"({"ok":true})");
    const std::string w = R"({"event":"worker_status","data":{"id":"cam1","type":"cam","actual_state":"running","metrics":{"n":1}}})";
    send_line(c, w);
    send_line(c, w);  // 같은 상태, 간격 안: 안 남김
    send_line(c, R"({"event":"command","data":{"op":"start","id":"cam1","ok":true}})");
    send_line(c, R"({"event":"system","data":{"clients":{"rest":true}}})");
    send_line(c, "broken line");
    std::this_thread::sleep_for(500ms);
    send_line(c, w);  // 간격 지남: 남김
    std::this_thread::sleep_for(300ms);
    kill(pid, SIGTERM);
    CHECK(wait_exit(pid, 5000) == 0);
    close(c);
    close(lfd);

    const auto rs = records(rec);
    CHECK(count(rs, "logger", "start") == 1);
    CHECK(count(rs, "logger", "connected") == 1);
    CHECK(count(rs, "worker") == 2);
    CHECK(count(rs, "command") == 1);
    CHECK(count(rs, "system") == 1);
    CHECK(count(rs, "logger", "stop") == 1);
    CHECK(!rs.empty() && rs.back().value("event", "") == "stop");
    for (const auto& r : rs) {
        CHECK(r.is_object() && r.contains("t") && r.contains("t_mono_ns"));
        if (r.value("type", "") == "worker") CHECK(r.value("worker_type", "") == "cam" && r["metrics"]["n"] == 1);
    }
}

static void test_reconnect() {
    const std::string sock = g_dir + "/pm2.sock", rec = g_dir + "/rec2";
    int lfd = listen_unix(sock);
    const pid_t pid = spawn(config(rec, sock));
    int c = accept_for(lfd, 5000);
    read_line(c, 2000);
    send_line(c, R"({"ok":true})");
    std::this_thread::sleep_for(200ms);
    close(c);  // process-manager가 끊김
    c = accept_for(lfd, 5000);  // 2 s 뒤 다시 붙음
    CHECK(c >= 0);
    read_line(c, 2000);
    send_line(c, R"({"ok":true})");
    std::this_thread::sleep_for(200ms);
    kill(pid, SIGTERM);
    CHECK(wait_exit(pid, 5000) == 0);
    close(c);
    close(lfd);
    const auto rs = records(rec);
    CHECK(count(rs, "logger", "connected") == 2);
    CHECK(count(rs, "logger", "disconnected") == 1);
}

static void test_bad_config() {
    const std::string rec = g_dir + "/rec3";
    CHECK(wait_exit(spawn(config(rec, "/nonexistent.sock", R"(,"nope":1)")), 5000) == 1);
    CHECK(wait_exit(spawn(config(rec, "/nonexistent.sock", R"(,"max_total_mb":1,"max_file_mb":5)")), 5000) == 1);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_worker [emulator...] <record-manager>\n");
        return 2;
    }
    for (int i = 1; i < argc; ++i) g_cmd.push_back(argv[i]);
    char tmpl[] = "/tmp/rec-it-XXXXXX";
    g_dir = mkdtemp(tmpl);
    signal(SIGPIPE, SIG_IGN);
    test_basic();
    test_reconnect();
    test_bad_config();
    (void)!std::system(("rm -rf " + g_dir).c_str());
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("record-manager: all tests passed\n");
    return 0;
}
