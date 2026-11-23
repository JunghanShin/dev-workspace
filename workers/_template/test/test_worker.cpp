// 워커 통합 시험: 실제 실행 파일을 띄운다(연동 ON·OFF 빌드 모두).
//   1) 워커 하나: 명령·상태·워커 안 연결(source → 포트 → processor)
//   2) 워커 둘(a: 생산, b: peer = a): 워커 간 데이터 버스(순서대로 전부)·스트림 구독·명령, a가 다시 시작해도 b가 다시 붙음
//   3) 가짜 장치 모드: 설정 "sim"이면 가짜 장치 프로그램을 자식으로 띄우고 상태에 표시
//   4) 기본 안전망·장애 주입(WORKER_FAULT_INJECTION=1): 예외는 세고 계속, 짧은 멈춤은 견딤, 연속 예외·긴 멈춤·Fatal은
//      safe_state 뒤 exit 4, 단계 설정("stages")과 runtime 상태
//
//   test_worker [에뮬레이터...] <example-worker 경로>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "worker/client.hpp"
#include "worker/workspace.hpp"

using Json = nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static std::vector<std::string> g_cmd;  // [에뮬레이터...] 실행 파일
static std::string g_dir;
// 시험이 중간에 끝나도 띄운 워커를 남기지 않는다(ctest가 기다리며 멈추지 않게)
static std::vector<pid_t> g_pids;
static void cleanup() {
    for (pid_t p : g_pids) kill(p, SIGKILL);
}

static bool wait_until(const std::function<bool()>& f, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return f();
}

static pid_t start(const std::string& id, const Json& config) {
    const std::string path = g_dir + "/" + id + ".json";
    std::ofstream(path) << Json{{"id", id}, {"config_version", 1}, {"config", config}}.dump();
    const pid_t pid = fork();
    if (pid == 0) {
        std::vector<std::string> args = g_cmd;
        args.push_back("--config");
        args.push_back(path);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    g_pids.push_back(pid);
    return pid;
}

static int stop(pid_t pid) {
    kill(pid, SIGTERM);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

// 끝나기를 기다린다(죽이지 않음). 끝났으면 종료 코드, 시간이 지나면 -1
static int wait_exit(pid_t pid, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
}

static Json cmd(const std::string& id, const std::string& c, const Json& args = Json::object()) {
    return worker::send_command(worker::default_command_socket(id), c, args, std::chrono::milliseconds(3000));
}

static Json status(const std::string& id) {
    const Json r = cmd(id, "get_status");
    return r.value("ok", false) && r["data"].is_object() ? r["data"] : Json::object();
}

// 모듈 칸 하나(없으면 빈 객체): status(id)의 칸을 안전하게
static Json mod(const std::string& id, const char* m) {
    const Json s = status(id);
    return s.contains(m) && s[m].is_object() ? s[m] : Json::object();
}


int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) g_cmd.push_back(argv[i]);
    if (g_cmd.empty()) {
        std::fprintf(stderr, "usage: test_worker [emulator...] <example-worker>\n");
        return 2;
    }
    char tmpl[] = "/tmp/example_worker_test_XXXXXX";
    g_dir = mkdtemp(tmpl);
    std::atexit(cleanup);
    std::set_terminate([] { cleanup(); std::abort(); });
    // 소켓 기본 위치를 시험 폴더로(연동 ON: WORKSPACE_DATA/run/..., OFF: XDG_RUNTIME_DIR/...). heartbeat는 없음
    setenv("WORKSPACE_DATA", g_dir.c_str(), 1);
    setenv("XDG_RUNTIME_DIR", g_dir.c_str(), 1);
    unsetenv("WORKSPACE_NOTIFY_SOCKET");
    for (const char* d : {"/run", "/run/cmd", "/run/stream", "/run/frames"}) mkdir((g_dir + d).c_str(), 0755);

    // 0) --io: 실행 파일에 넣은 입출력 정의(설정·소켓 없이 출력하고 끝)
    {
        std::string line;
        for (const auto& a : g_cmd) line += "'" + a + "' ";
        FILE* p = popen((line + "--io").c_str(), "r");
        CHECK(p != nullptr);
        std::string text;
        char buf[4096];
        size_t n;
        while (p && (n = fread(buf, 1, sizeof buf, p)) > 0) text.append(buf, n);
        CHECK(p && pclose(p) == 0);
        const Json io = Json::parse(text, nullptr, false);
        CHECK(io.is_object());
        CHECK(io.value("common", Json::object()).value("name", "") == "common");
        CHECK(io.value("self", Json::object()).value("name", "") == "self");
        CHECK(io.value("provides", Json::object()).contains("example"));
        CHECK(io["uses"]["upstream"]["definition"].value("name", "") == "example");
    }

    // 1) 워커 하나
    pid_t a = start("a", {{"source", {{"rate_hz", 50}, {"size", 16}}}, {"processor", {{"window", 10}}}});
    CHECK(wait_until([] { return status("a").contains("source"); }, 8000));
    CHECK(wait_until([] { return mod("a", "processor").value("total", 0) >= 5; }, 5000));  // 포트로 받음
    CHECK(cmd("a", "source.set_rate", {{"hz", 0}})["error"]["code"] == "bad_request");
    CHECK(cmd("a", "nope.x")["ok"] == false);
    CHECK(mod("a", "peer").value("peer", std::string("x")).empty());  // peer 비움 = 꺼짐

    // 2) 워커 둘: b가 a의 데이터·상태를 받고, a에 명령한다
    pid_t b = start("b", {{"source", {{"rate_hz", 5}}}, {"peer", {{"peer", "a"}}}});
    CHECK(wait_until([] {
        const Json p = mod("b", "peer");
        return p.value("received", 0) >= 20 && p.value("stream_link", false) && p.value("data_link", false);
    }, 10000));
    CHECK(mod("b", "peer").value("lost", 1) == 0);           // 순서대로 전부: 빠짐 없음
    CHECK(mod("b", "peer").value("peer_topics", Json::object()).contains("source"));  // 스트림 주제를 받음
    const Json r = cmd("b", "peer.set_rate", {{"hz", 20}});      // 워커 간 요청·응답(비동기, 응답은 a의 것)
    CHECK(r.value("ok", false) && r.value("data", Json::object()).value("rate_hz", 0.0) == 20.0);
    CHECK(wait_until([] { return mod("a", "source").value("rate_hz", 0.0) == 20.0; }, 3000));

    // a가 다시 시작해도 b는 살아서 기다리다 다시 붙는다
    CHECK(stop(a) == 0);
    CHECK(wait_until([] { return !mod("b", "peer").value("data_link", true); }, 5000));
    const uint64_t before = mod("b", "peer").value("received", 0ull);
    a = start("a", {{"source", {{"rate_hz", 50}, {"size", 16}}}});
    CHECK(wait_until([&] {
        const Json p = mod("b", "peer");
        return p.value("data_link", false) && p.value("received", 0ull) > before + 10;
    }, 10000));

    CHECK(stop(b) == 0);
    CHECK(stop(a) == 0);

    // 3) 가짜 장치 모드: 호스트가 가짜 장치 프로그램(실행 파일 옆 example-sim)을 띄우고 상태에 표시한다.
    //    에뮬레이터(QEMU)로 도는 Jetson 시험에서는 자식 실행이 다른 아키텍처라 건너뛴다
    if (g_cmd.size() == 1) {
        pid_t s = start("s", {{"sim", {{"cmd", "@EXEC_DIR@/example-sim 0"}}}});
        CHECK(wait_until([] {
            const Json st = status("s");
            return st.contains("sim") && st["sim"].value("running", false);
        }, 8000));
        CHECK(stop(s) == 0);  // 워커가 끝나면 가짜 장치도 함께 끝난다(Host가 kill_wait)
    }
    // 4) 기본 안전망·장애 주입(시험 빌드 전용 명령 __fault)
    setenv("WORKER_FAULT_INJECTION", "1", 1);
    const Json fcfg = {{"source", {{"rate_hz", 50}, {"size", 8}}}, {"stages", {{"source.gen", {{"queue", 16}}}}},
                       {"thread_stall_sec", 1}, {"watchdog_sec", 2}};
    pid_t f = start("f", fcfg);
    CHECK(wait_until([] { return status("f").contains("runtime"); }, 8000));
    {
        const Json rt = status("f")["runtime"];
        CHECK(rt["stages"]["source.gen"]["queue"] == 16 && rt["stages"]["source.gen"]["overflow"] == "drop_oldest");
        CHECK(rt.value("fault_injection", false) && rt["loop"]["budget_ms"] == 50);
    }
    CHECK(cmd("f", "__fault", {{"kind", "throw"}, {"where", "timer"}, {"count", 2}}).value("ok", false));
    CHECK(wait_until([] { return status("f").value("runtime", Json::object()).value("errors", 0) >= 2; }, 3000));
    const uint64_t seq0 = mod("f", "source").value("seq", 0ull);
    CHECK(wait_until([&] { return mod("f", "source").value("seq", 0ull) > seq0 + 5; }, 3000));  // 예외 뒤에도 계속
    CHECK(cmd("f", "__fault", {{"kind", "stall"}, {"stage", "source.gen"}, {"ms", 400}}).value("ok", false));
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(wait_exit(f, 10) == -1);  // 판정(1 s)보다 짧은 멈춤은 견딘다
    const uint64_t seq1 = mod("f", "source").value("seq", 0ull);
    CHECK(wait_until([&] { return mod("f", "source").value("seq", 0ull) > seq1 + 5; }, 3000));
    CHECK(cmd("f", "__fault", {{"kind", "throw"}, {"where", "timer"}, {"count", 3}}).value("ok", false));
    CHECK(wait_exit(f, 5000) == 4);  // 연달아 3번: 상태가 깨졌다고 보고 safe_state 뒤 exit 4

    pid_t g = start("g", fcfg);
    CHECK(wait_until([] { return status("g").contains("runtime"); }, 8000));
    CHECK(cmd("g", "__fault", {{"kind", "stall"}, {"stage", "source.gen"}, {"ms", 5000}}).value("ok", false));
    CHECK(wait_exit(g, 6000) == 4);  // 긴 멈춤: 워치독(SIGALRM)보다 먼저 safe_state 뒤 exit 4

    pid_t h = start("h", fcfg);
    CHECK(wait_until([] { return status("h").contains("runtime"); }, 8000));
    CHECK(cmd("h", "__fault", {{"kind", "fatal"}}).value("ok", false));
    CHECK(wait_exit(h, 5000) == 4);  // worker::Fatal
    unsetenv("WORKER_FAULT_INJECTION");
    pid_t n = start("n", fcfg);      // 시험 빌드 변수가 없으면 장애 주입 명령은 없다
    CHECK(wait_until([] { return status("n").contains("runtime"); }, 8000));
    CHECK(cmd("n", "__fault", {{"kind", "fatal"}})["ok"] == false);
    CHECK(stop(n) == 0);

    std::string rm = "rm -rf " + g_dir;
    (void)std::system(rm.c_str());
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all worker tests passed\n");
    return 0;
}
