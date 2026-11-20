// 워커 통합 시험: 실제 replay-worker를 띄운다(연동 ON·OFF 빌드 모두).
//   1) 가짜 생산자(이 시험 안의 framebus::Producer, 워커 id "src")의 데이터 버스를 녹화(record_start/record_stop)
//   2) 목록·같은 이름 거절
//   3) 재생(play_start, 2배속)을 받는 쪽(Consumer InOrder)이 녹화한 프레임을 같은 번호·같은 내용·같은 순서로 받는지
//   4) 삭제, SIGTERM 정리
//
//   test_worker [에뮬레이터...] <replay-worker 경로>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "framebus/framebus.hpp"
#include "worker/client.hpp"
#include "worker/workspace.hpp"

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

static pid_t g_pid = -1;
static void cleanup() {
    if (g_pid > 0) kill(g_pid, SIGKILL);
}

static bool wait_until(const std::function<bool()>& f, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(50ms);
    }
    return f();
}

static Json cmd(const std::string& c, const Json& args = Json::object()) {
    return worker::send_command(worker::default_command_socket("rp"), c, args, std::chrono::milliseconds(3000));
}
static Json st() {
    const Json r = cmd("get_status");
    return r.value("ok", false) && r["data"].contains("replay") ? r["data"]["replay"] : Json::object();
}

static uint8_t pattern(uint64_t id, size_t i) { return static_cast<uint8_t>(id * 7 + i); }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_worker [emulator...] <replay-worker>\n");
        return 2;
    }
    char tmpl[] = "/tmp/replay_worker_test_XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    std::atexit(cleanup);
    setenv("WORKSPACE_DATA", dir.c_str(), 1);
    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
    unsetenv("WORKSPACE_NOTIFY_SOCKET");
    for (const char* d : {"/run", "/run/cmd", "/run/stream", "/run/frames"}) mkdir((dir + d).c_str(), 0755);

    // 가짜 생산자 "src": 64바이트 원시 레코드, 100 Hz
    framebus::Producer src(worker::default_socket("src", "frames"), framebus::Format::raw(64, "u8x64"), 8);
    std::string err;
    CHECK(src.start(&err));
    std::atomic<bool> stop_src{false};
    std::atomic<uint64_t> sent{0};
    std::thread prod([&] {
        uint64_t id = 0;
        while (!stop_src) {
            if (auto s = src.acquire()) {
                ++id;
                for (size_t i = 0; i < 64; ++i) s->data[i] = pattern(id, i);
                src.publish(*s, id, static_cast<int64_t>(id) * 10000000, 64);
                sent = id;
            }
            std::this_thread::sleep_for(10ms);
        }
    });

    const std::string cfg = dir + "/rp.json";
    std::ofstream(cfg) << Json{{"id", "rp"}, {"config_version", 1}, {"config", {{"replay", {{"max_total_mb", 100}}}}}}.dump();
    g_pid = fork();
    if (g_pid == 0) {
        std::vector<std::string> args(argv + 1, argv + argc);
        args.push_back("--config");
        args.push_back(cfg);
        std::vector<char*> av;
        for (auto& a : args) av.push_back(a.data());
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    CHECK(wait_until([] { return st().contains("recording"); }, 8000));

    // 1) 녹화
    Json r = cmd("replay.record_start", {{"source", "src"}, {"name", "t1"}});
    CHECK(r.value("ok", false));
    CHECK(cmd("replay.record_start", {{"source", "src"}, {"name", "t2"}})["error"]["code"] == "busy");  // 하나만
    CHECK(wait_until([] { return st().value("recording", Json::object()).value("frames", 0) >= 60; }, 8000));
    CHECK(st().value("recording", Json::object()).value("link", false));
    r = cmd("replay.record_stop");
    CHECK(r.value("ok", false));
    stop_src = true;
    prod.join();
    src.stop();

    // 2) 목록, 같은 이름 거절
    r = cmd("replay.list");
    CHECK(r.value("ok", false) && r["data"]["recordings"].size() == 1);
    const Json rec = r["data"]["recordings"][0];
    const uint64_t n = rec.value("frames", 0ull);
    CHECK(rec.value("name", "") == "t1" && rec.value("source", "") == "src" && n >= 60 && rec.value("pixel", "") == "u8x64");
    CHECK(cmd("replay.record_start", {{"source", "src"}, {"name", "t1"}})["error"]["code"] == "bad_request");

    // 3) 재생: 받는 쪽이 같은 번호·같은 내용·같은 순서로 전부 받는다
    framebus::Consumer c(worker::default_socket("rp", "frames"), framebus::Consumer::Mode::InOrder);
    r = cmd("replay.play_start", {{"name", "t1"}, {"speed", 2.0}});
    CHECK(r.value("ok", false));
    CHECK(wait_until([&] { return c.connect(&err); }, 5000));
    CHECK(c.format().pixel == "u8x64");
    std::vector<uint64_t> got;
    bool content_ok = true;
    const auto end = std::chrono::steady_clock::now() + 15s;
    while (got.size() < n && std::chrono::steady_clock::now() < end) {
        framebus::Frame f;
        if (c.next(200, &f) != framebus::Consumer::Result::Frame) continue;
        if (f.bytes != 64) content_ok = false;
        for (size_t i = 0; i < f.bytes; ++i)
            if (f.data[i] != pattern(f.frame_id, i)) content_ok = false;
        got.push_back(f.frame_id);
        c.release(f);
    }
    CHECK(got.size() == n);
    CHECK(content_ok);
    bool ordered = true;
    for (size_t i = 1; i < got.size(); ++i)
        if (got[i] <= got[i - 1]) ordered = false;
    CHECK(ordered);
    CHECK(wait_until([] { return !st().value("playing", Json::object()).value("active", true); }, 5000));  // 끝까지 재생하고 멈춤
    CHECK(cmd("replay.play_start", {{"name", "none"}})["error"]["code"] == "bad_request");

    // 4) 삭제, 정리
    CHECK(cmd("replay.delete", {{"name", "t1"}}).value("ok", false));
    CHECK(cmd("replay.list")["data"]["recordings"].empty());
    kill(g_pid, SIGTERM);
    int status = 0;
    waitpid(g_pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    g_pid = -1;
    std::string rm = "rm -rf " + dir;
    (void)std::system(rm.c_str());
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all replay worker tests passed\n");
    return 0;
}
