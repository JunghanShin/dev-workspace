// record-manager 통합 시험: 실제 실행 파일을 가짜 process-manager 소켓에 붙여 기록 파일을 본다.
//   1) start → connected → worker(바뀜·간격)·command·system → SIGTERM → stop, exit 0. 줄 모양은 예전 logger와 같음
//   2) process-manager가 끊김 → disconnected 한 줄, 다시 붙으면 connected
//   3) 잘못된 설정·모르는 키 → exit 1
//   4) 여러 워커 입력(입력 파일 many.streams): 워커 스트림의 주제(솎기·kf)·이벤트를 남기고, 목록에서 빠지면 구독을 끊음
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

static pid_t spawn(const std::string& config, const std::vector<std::pair<std::string, std::string>>& env = {}) {
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
        unsetenv("WORKSPACE_INPUTS_FILE");
        for (const auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        execvp(argv[0], argv.data());  // 에뮬레이터는 이름만 올 수 있다(PATH에서 찾음)
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
        if (r.is_object() && r.value("type", "") == type && (event.empty() || r.value("event", "") == event)) ++n;
    return n;
}

static std::string config(const std::string& rec_dir, const std::string& sock, const std::string& extra = "") {
    return R"({"id":"record-manager","config_version":1,"config":{"record":{"dir":")" + rec_dir + R"(","manager_socket":")" +
           sock + R"(","interval_s":1.0)" + extra + "}}}";
}

static void test_basic() {
    const std::string sock = g_dir + "/pm.sock", rec = g_dir + "/rec1";
    const int lfd = listen_unix(sock);
    CHECK(lfd >= 0);
    const pid_t pid = spawn(config(rec, sock));
    const int c = accept_for(lfd, 15000);  // 에뮬레이터(QEMU)에서는 뜨는 데 몇 초
    CHECK(c >= 0);
    const Json sub = Json::parse(read_line(c, 10000), nullptr, false);
    CHECK(sub.is_object() && sub.value("op", "") == "subscribe" && sub["topics"].size() == 3 &&
          sub.value("period_ms", 0) == 1000);
    send_line(c, R"({"ok":true})");
    const std::string w = R"({"event":"worker_status","data":{"id":"cam1","type":"cam","actual_state":"running","metrics":{"n":1}}})";
    send_line(c, w);
    send_line(c, w);  // 같은 상태, 간격 안: 안 남김
    send_line(c, R"({"event":"command","data":{"op":"start","id":"cam1","ok":true}})");
    send_line(c, R"({"event":"system","data":{"clients":{"rest":true}}})");
    send_line(c, "broken line");
    std::this_thread::sleep_for(1500ms);
    send_line(c, w);  // 간격(1 s) 지남: 남김
    std::this_thread::sleep_for(1000ms);
    kill(pid, SIGTERM);
    CHECK(wait_exit(pid, 15000) == 0);
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
        if (r.is_object() && r.value("type", "") == "worker") CHECK(r.value("worker_type", "") == "cam" && r["metrics"]["n"] == 1);
    }
}

static void test_reconnect() {
    const std::string sock = g_dir + "/pm2.sock", rec = g_dir + "/rec2";
    int lfd = listen_unix(sock);
    const pid_t pid = spawn(config(rec, sock));
    int c = accept_for(lfd, 15000);
    read_line(c, 10000);
    send_line(c, R"({"ok":true})");
    std::this_thread::sleep_for(200ms);
    close(c);  // process-manager가 끊김
    c = accept_for(lfd, 15000);  // 2 s 뒤 다시 붙음
    CHECK(c >= 0);
    read_line(c, 10000);
    send_line(c, R"({"ok":true})");
    std::this_thread::sleep_for(1000ms);
    kill(pid, SIGTERM);
    CHECK(wait_exit(pid, 15000) == 0);
    close(c);
    close(lfd);
    const auto rs = records(rec);
    CHECK(count(rs, "logger", "connected") == 2);
    CHECK(count(rs, "logger", "disconnected") == 1);
}

static void write_inputs(const std::string& path, const std::string& many) {
    const std::string tmp = path + ".tmp";
    std::ofstream(tmp) << R"({"inputs": {}, "many": {"streams": )" + many + "}}";
    ::rename(tmp.c_str(), path.c_str());
}

static void test_streams() {
    const std::string data = g_dir + "/data", rec = g_dir + "/rec4", pm = g_dir + "/pm4.sock", inputs = g_dir + "/inputs.json";
    (void)!std::system(("mkdir -p " + data + "/run/stream").c_str());
    const int pfd = listen_unix(pm);
    const int sfd = listen_unix(data + "/run/stream/cam1.sock");
    CHECK(pfd >= 0 && sfd >= 0);
    write_inputs(inputs, R"([{"worker": "cam1"}, {"worker": "record-manager"}])");  // 자기 자신은 뺀다
    const pid_t pid = spawn(config(rec, pm), {{"WORKSPACE_DATA", data}, {"WORKSPACE_INPUTS_FILE", inputs}});
    const int pc = accept_for(pfd, 15000);
    read_line(pc, 10000);
    send_line(pc, R"({"ok":true})");
    const int sc = accept_for(sfd, 15000);
    CHECK(sc >= 0);
    send_line(sc, R"({"seq":1,"kf":true,"t":1.0,"topics":{"camera":{"fps":30,"t_mono_ns":123},"mount":{"yaw":2}},)"
                  R"("events":[{"t_mono_ns":99,"source":"cam1.link","severity":"warning","code":"link_down","text":"x"}]})");
    send_line(sc, R"({"seq":2,"kf":false,"t":1.1,"topics":{"camera":{"fps":29}}})");  // 간격(1 s) 안: 안 남김
    std::this_thread::sleep_for(1000ms);
    // 목록에서 빠짐 → 1 s 안에 파일을 다시 읽고 구독을 끊는다(상대 쪽에서 끊김이 보임)
    write_inputs(inputs, "[]");
    char c;
    pollfd p{sc, POLLIN, 0};
    bool closed = false;
    for (int i = 0; i < 60 && !closed; ++i)
        if (poll(&p, 1, 100) > 0) closed = read(sc, &c, 1) == 0;
    CHECK(closed);
    kill(pid, SIGTERM);
    CHECK(wait_exit(pid, 15000) == 0);
    close(sc);
    close(pc);
    close(sfd);
    close(pfd);
    const auto rs = records(rec);
    CHECK(count(rs, "topic") == 2);
    CHECK(count(rs, "event") == 1);
    for (const auto& r : rs) {
        if (!r.is_object()) continue;
        if (r.value("type", "") == "topic" && r.value("topic", "") == "camera")
            CHECK(r["t_mono_ns"] == 123 && r["src"] == "cam1" && r["kf"] == true && r["v"]["fps"] == 30);
        if (r.value("type", "") == "event") CHECK(r["t_mono_ns"] == 99 && r["v"]["code"] == "link_down");
    }
}

static void test_bad_config() {
    const std::string rec = g_dir + "/rec3";
    CHECK(wait_exit(spawn(config(rec, "/nonexistent.sock", R"(,"nope":1)")), 15000) == 1);
    CHECK(wait_exit(spawn(config(rec, "/nonexistent.sock", R"(,"max_total_mb":1,"max_file_mb":5)")), 15000) == 1);
    CHECK(wait_exit(spawn(config(rec, "/nonexistent.sock", R"(,"topics":[{"interval_ms":5}])")), 15000) == 1);
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
    test_streams();
    test_bad_config();
    (void)!std::system(("rm -rf " + g_dir).c_str());
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("record-manager: all tests passed\n");
    return 0;
}
