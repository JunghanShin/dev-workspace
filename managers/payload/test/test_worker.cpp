// payload-manager 통합 시험: 실제 실행 파일을 가짜 process-manager(구독 + start·stop 요청)와 가짜 status 주제에 붙인다.
//   1) payload.list: 매니저가 아닌 워커만, status의 건강이 들어감
//   2) payload.power: process-manager에 stop 요청, 답에 desired_state. 매니저 대상은 forbidden
//   3) payload.power_group: 묶음의 모듈마다 start 요청, 모듈별 결과
//   4) 잘못된 설정 → exit 1
//
//   test_worker [에뮬레이터...] <payload-manager 경로>
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

static std::string worker_line(const std::string& id, const std::string& kind) {
    return Json{{"event", "worker_status"},
                {"data", {{"id", id}, {"kind", kind}, {"actual_state", "running"}, {"desired_state", "running"}, {"autostart", true}}}}
        .dump();
}

// 가짜 process-manager: subscribe면 워커 상태를 0.3 s마다 보내고, start·stop이면 기록하고 desired_state로 답한다
struct FakePm {
    int lfd = -1;
    std::atomic<bool> stop{false};
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
                for (const auto& [id, kind] : std::vector<std::pair<std::string, std::string>>{
                         {"cam1", "worker"}, {"cam2", "worker"}, {"sensor", "worker"}, {"mission-manager", "manager"}})
                    send_line(c, worker_line(id, kind));
                for (int i = 0; i < 6 && !stop; ++i) std::this_thread::sleep_for(50ms);
            }
        } else if (op == "start" || op == "stop") {
            {
                std::lock_guard<std::mutex> lock(mu);
                writes.push_back(req);
            }
            send_line(c, Json{{"ok", true}, {"data", {{"id", req["id"]}, {"desired_state", op == "start" ? "running" : "stopped"}}}}.dump());
        } else {  // get_io 등: 계약 없음
            send_line(c, R"({"ok":false,"error":{"code":"not_found","message":"no io"}})");
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

static Json cmd(const std::string& sock, const Json& req) {
    const int fd = connect_unix(sock);
    if (fd < 0) return Json();
    send_line(fd, req.dump());
    const Json r = Json::parse(read_line(fd, 8000), nullptr, false);
    close(fd);
    return r.is_object() ? r : Json::object();
}

static void test_payload() {
    const std::string data = g_dir + "/data", pm = g_dir + "/pm.sock", st = g_dir + "/status.sock", inputs = g_dir + "/inputs.json";
    (void)!std::system(("mkdir -p " + data + "/run/cmd " + data + "/run/stream").c_str());
    FakePm fake;
    fake.start(pm);
    // 가짜 status-manager 주제: 붙으면 system 한 줄
    const int sfd = listen_unix(st);
    std::atomic<bool> sstop{false};
    std::thread status([&] {
        while (!sstop) {
            pollfd p{sfd, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            const int c = accept4(sfd, nullptr, nullptr, SOCK_CLOEXEC);
            send_line(c, R"({"seq":1,"kf":true,"t":1,"topics":{"system":{"health":"degraded","workers":{"cam1":{"health":"degraded","why":"link down"}}}}})");
            while (!sstop) std::this_thread::sleep_for(50ms);
            close(c);
        }
    });
    std::ofstream(inputs) << Json{{"inputs", {{"system", {{"stream", st}}}}}}.dump();
    const pid_t pid = spawn(R"({"id":"payload-manager","config_version":1,"config":{"payload":{"groups":{"cams":["cam*"]}}}})",
                            {{"WORKSPACE_DATA", data}, {"WORKSPACE_MANAGER_SOCKET", pm}, {"WORKSPACE_INPUTS_FILE", inputs}});
    const std::string sock = data + "/run/cmd/payload-manager.sock";
    Json list;
    for (int i = 0; i < 150; ++i) {  // 뜨고, 워커 상태·건강이 들어올 때까지
        list = cmd(sock, {{"cmd", "payload.list"}, {"args", Json::object()}});
        if (list.is_object() && list.value("ok", false) && list["data"]["modules"].contains("cam1") &&
            list["data"]["modules"]["cam1"]["health"] == "degraded")
            break;
        std::this_thread::sleep_for(100ms);
    }
    CHECK(list.is_object() && list.value("ok", false));
    if (!list.is_object()) list = Json::object();
    const Json mods = list.value("data", Json::object()).value("modules", Json::object());
    CHECK(mods.contains("cam1") && mods.contains("sensor") && !mods.contains("mission-manager"));
    CHECK(mods.contains("cam1") && mods["cam1"]["health"] == "degraded" && mods["cam1"]["groups"] == Json::parse(R"(["cams"])"));
    // 켜고 끄기
    Json r = cmd(sock, {{"cmd", "payload.power"}, {"args", {{"module", "sensor"}, {"on", false}}}, {"from", "screen"}});
    CHECK(r.value("ok", false) && r["data"]["desired_state"] == "stopped");
    r = cmd(sock, {{"cmd", "payload.power"}, {"args", {{"module", "mission-manager"}, {"on", false}}}});
    CHECK(r.contains("error") && r["error"]["code"] == "forbidden");
    r = cmd(sock, {{"cmd", "payload.power"}, {"args", {{"module", "nope"}, {"on", true}}}});
    CHECK(r.contains("error") && r["error"]["code"] == "not_found");
    r = cmd(sock, {{"cmd", "payload.power_group"}, {"args", {{"group", "cams"}, {"on", true}}}});
    CHECK(r.value("ok", false) && r["data"]["results"]["cam1"]["ok"] == true && r["data"]["results"]["cam2"]["desired_state"] == "running");
    {
        std::lock_guard<std::mutex> lock(fake.mu);
        CHECK(fake.writes.size() == 3);
        CHECK(fake.writes.size() == 3 && fake.writes[0] == Json::parse(R"({"op":"stop","id":"sensor"})"));
    }
    kill(pid, SIGTERM);
    CHECK(wait_exit(pid, 15000) == 0);
    sstop = true;
    status.join();
    close(sfd);
    fake.finish();
}

static void test_bad_config() {
    CHECK(wait_exit(spawn(R"({"id":"payload-manager","config":{"payload":{"nope":1}}})", {}), 15000) == 1);
    CHECK(wait_exit(spawn(R"({"id":"payload-manager","config":{"payload":{"groups":{"g":"cam*"}}}})", {}), 15000) == 1);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_worker [emulator...] <payload-manager>\n");
        return 2;
    }
    for (int i = 1; i < argc; ++i) g_cmd.push_back(argv[i]);
    char tmpl[] = "/tmp/pl-it-XXXXXX";
    g_dir = mkdtemp(tmpl);
    signal(SIGPIPE, SIG_IGN);
    test_payload();
    test_bad_config();
    (void)!std::system(("rm -rf " + g_dir).c_str());
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("payload-manager: all tests passed\n");
    return 0;
}
