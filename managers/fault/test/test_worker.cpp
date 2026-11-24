// fault-manager 통합 시험: 실제 실행 파일을 가짜 status 주제(system)와 가짜 mission 명령 소켓에 붙인다.
//   1) cam1 건강 fault가 for_s(1 s) 넘게 이어짐 → 이상 올림, mission.send 한 번(대응), fault.list에 보임
//   2) fault.ack → acked
//   3) 건강이 ok로 → 이상 내림
//   4) 잘못된 규칙 → exit 1
//
//   test_worker [에뮬레이터...] <fault-manager 경로>
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
        unsetenv("WORKSPACE_INPUTS_FILE");
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

static Json cmd(const std::string& sock, const Json& req) {
    const int fd = connect_unix(sock);
    if (fd < 0) return Json::object();
    send_line(fd, req.dump());
    const Json r = Json::parse(read_line(fd, 8000), nullptr, false);
    close(fd);
    return r.is_object() ? r : Json::object();
}

static void test_fault() {
    const std::string data = g_dir + "/data", st = g_dir + "/status.sock", ms = g_dir + "/mission.sock", inputs = g_dir + "/in.json";
    (void)!std::system(("mkdir -p " + data + "/run/cmd " + data + "/run/stream").c_str());
    std::atomic<bool> stop{false};
    std::atomic<int> cam_fault{1};
    // 가짜 status 주제: 0.2 s마다 system
    const int sfd = listen_unix(st);
    std::thread status([&] {
        while (!stop) {
            pollfd p{sfd, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            const int c = accept4(sfd, nullptr, nullptr, SOCK_CLOEXEC);
            while (!stop) {
                const std::string h = cam_fault ? "fault" : "ok";
                send_line(c, Json{{"seq", 1}, {"kf", true}, {"t", 1},
                                  {"topics", {{"system", {{"health", "degraded"}, {"workers", {{"cam1", {{"health", h}}}}}}}}}}.dump());
                std::this_thread::sleep_for(200ms);
            }
            close(c);
        }
    });
    // 가짜 mission: 받은 요청 기록, ok로 답
    std::mutex mu;
    std::vector<Json> got;
    const int mfd = listen_unix(ms);
    std::thread mission([&] {
        while (!stop) {
            pollfd p{mfd, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            const int c = accept4(mfd, nullptr, nullptr, SOCK_CLOEXEC);
            const Json req = Json::parse(read_line(c, 3000), nullptr, false);
            {
                std::lock_guard<std::mutex> lock(mu);
                got.push_back(req);
            }
            send_line(c, R"({"ok":true,"data":{"done":true}})");
            close(c);
        }
    });
    std::ofstream(inputs) << Json{{"inputs", {{"system", {{"stream", st}}}, {"mission", {{"command", ms}}}}}}.dump();
    const std::string rules = R"([{"id":"cam_lost","when":{"health":{"worker":"cam*","is":["fault"]}},"for_s":1,"severity":"error",
                                   "do":[{"target":"payload-manager","cmd":"payload.power","args":{"module":"cam1","on":true}}],
                                   "stages":[{"after_s":2,"severity":"critical","do":[{"target":"payload-manager","cmd":"payload.stop","args":{"module":"cam1"}}]}]},
                                  {"id":"both","when":{"all":[{"health":{"worker":"cam*","is":"fault"}},{"system":{"is":"degraded"}}]},"severity":"warning"}])";
    const pid_t pid = spawn(R"({"id":"fault-manager","config_version":1,"config":{"fault":{"rules":)" + rules + "}}}",
                            {{"WORKSPACE_DATA", data}, {"WORKSPACE_MANAGER_SOCKET", g_dir + "/no-pm.sock"}, {"WORKSPACE_INPUTS_FILE", inputs}});
    const std::string sock = data + "/run/cmd/fault-manager.sock";
    Json list;
    for (int i = 0; i < 150; ++i) {
        list = cmd(sock, {{"cmd", "fault.list"}, {"args", Json::object()}});
        if (list.value("ok", false) && list["data"]["active"].contains("cam_lost:cam1")) break;
        std::this_thread::sleep_for(100ms);
    }
    CHECK(list.value("ok", false) && list["data"]["active"].contains("cam_lost:cam1"));
    CHECK(list.value("ok", false) && list["data"]["worst"] == "error");
    CHECK(list.value("ok", false) && list["data"]["active"].contains("both"));  // 조합(시스템 전체)
    // 단계: 올라간 뒤 2 s → critical, 그 단계 대응 한 번. 그 뒤로 되풀이하지 않음
    for (int i = 0; i < 50; ++i) {
        list = cmd(sock, {{"cmd", "fault.list"}, {"args", Json::object()}});
        if (list.value("ok", false) && list["data"]["active"]["cam_lost:cam1"].value("stage", 0) == 1) break;
        std::this_thread::sleep_for(100ms);
    }
    CHECK(list.value("ok", false) && list["data"]["active"]["cam_lost:cam1"]["stage"] == 1 && list["data"]["worst"] == "critical");
    std::this_thread::sleep_for(1000ms);
    {
        std::lock_guard<std::mutex> lock(mu);
        CHECK(got.size() == 2);
        CHECK(!got.empty() && got[0]["cmd"] == "mission.send" && got[0]["args"]["target"] == "payload-manager" &&
              got[0]["args"]["cmd"] == "payload.power" && got[0]["args"]["args"]["module"] == "cam1");
        CHECK(got.size() == 2 && got[1]["args"]["cmd"] == "payload.stop");
    }
    list = cmd(sock, {{"cmd", "fault.list"}, {"args", Json::object()}});
    CHECK(list.value("ok", false) && list["data"]["active"]["cam_lost:cam1"]["actions"][0]["ok"] == true);
    CHECK(list.value("ok", false) && list["data"]["active"]["cam_lost:cam1"]["actions"][1]["ok"] == true);
    const Json stj = cmd(sock, {{"cmd", "get_status"}, {"args", Json::object()}});
    CHECK(stj.value("ok", false) && stj["data"]["inputs"]["system"]["link"] == true && stj["data"]["inputs"]["mission"]["link"] == true);
    Json r = cmd(sock, {{"cmd", "fault.ack"}, {"args", {{"id", "cam_lost:cam1"}}}});
    CHECK(r.value("ok", false));
    r = cmd(sock, {{"cmd", "fault.ack"}, {"args", {{"id", "nope"}}}});
    CHECK(r.contains("error") && r["error"]["code"] == "bad_request");
    // 풀림 → 내림
    cam_fault = 0;
    bool cleared = false;
    for (int i = 0; i < 50 && !cleared; ++i) {
        list = cmd(sock, {{"cmd", "fault.list"}, {"args", Json::object()}});
        cleared = list.value("ok", false) && !list["data"]["active"].contains("cam_lost:cam1") && !list["data"]["active"].contains("both");
        std::this_thread::sleep_for(100ms);
    }
    CHECK(cleared);
    kill(pid, SIGTERM);
    CHECK(wait_exit(pid, 15000) == 0);
    stop = true;
    status.join();
    mission.join();
    close(sfd);
    close(mfd);
}

static void test_bad_config() {
    CHECK(wait_exit(spawn(R"({"id":"fault-manager","config":{"fault":{"nope":1}}})", {}), 15000) == 1);
    CHECK(wait_exit(spawn(R"({"id":"fault-manager","config":{"fault":{"rules":[{"id":"a","when":{}}]}}})", {}), 15000) == 1);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_worker [emulator...] <fault-manager>\n");
        return 2;
    }
    for (int i = 1; i < argc; ++i) g_cmd.push_back(argv[i]);
    char tmpl[] = "/tmp/ft-it-XXXXXX";
    g_dir = mkdtemp(tmpl);
    signal(SIGPIPE, SIG_IGN);
    test_fault();
    test_bad_config();
    (void)!std::system(("rm -rf " + g_dir).c_str());
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("fault-manager: all tests passed\n");
    return 0;
}
