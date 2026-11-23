// sysmon 통합 시험: 실제 실행 파일을 띄우고 heartbeat 소켓(가짜 Manager)으로 READY·WATCHDOG 지표를 받는다.
//   1) 직접 실행(가짜 tegrastats 셸 스크립트): 첫 줄 뒤 READY, 지표 이름·값, --interval 인자, SIGTERM → exit 0 + 자식 정리
//   2) 설정: 새 형식 {"sysmon": {"interval_ms"}}·옛 형식(맨 위 interval_ms), 잘못된 값·모르는 키 → exit 1
//   3) 장치 오류 exit 2: tegrastats 없음, 알아볼 수 없는 줄만, 비정상 종료, 출력 멈춤
//   4) root 수집기 파일: collector_root=1, EMC·전용 HW 지표, 파일이 멈추면 exit 2. 오래된 파일이면 직접 실행으로
//
//   test_worker [에뮬레이터...] <sysmon 경로>
//   에뮬레이터(QEMU)로 도는 Jetson 시험에서는 가짜 tegrastats(셸)와 PATH를 쓰는 시험을 건너뛴다(수집기·설정 시험만)
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

using Json = nlohmann::json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

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
static pid_t g_pid = -1;
static int g_sock = -1;
static void cleanup() {
    if (g_pid > 0) kill(g_pid, SIGKILL);
}

// 가짜 tegrastats: FAKE_MODE에 따라 줄을 낸다. 인자와 pid를 FAKE_DIR에 남긴다
static const char* kFake = R"(#!/bin/sh
echo "$*" > "$FAKE_DIR/args"
echo $$ > "$FAKE_DIR/pid"
trap 'exit 0' TERM
LINE='RAM 3456/7620MB (lfb 2x4MB) SWAP 12/3810MB (cached 0MB) CPU [11%@1497,off,2%@729,3%@729] GR3D_FREQ 0%@[305] cpu@46.937C soc2@44.281C tj@47.125C GPU@-256C VDD_IN 4524mW/4524mW VDD_CPU_GPU_CV 486mW/486mW'
sleep 0.6
case "$FAKE_MODE" in
  garbage) while :; do echo "hello from a strange tegrastats"; sleep 0.2; done ;;
  exit) echo "$LINE"; sleep 0.5; exit 3 ;;
  silent) echo "$LINE"; while :; do sleep 0.2; done ;;
  *) while :; do echo "$LINE"; sleep 0.2; done ;;
esac
)";

static std::string jp4_line(int emc) {
    return "RAM 2034/3964MB (lfb 4x4MB) SWAP 0/1982MB (cached 0MB) IRAM 0/252kB(lfb 252kB) CPU [5%@1479,2%@1479,4%@1479,1%@1479] "
           "EMC_FREQ " + std::to_string(emc) + "%@1600 GR3D_FREQ 9%@921 NVENC 1036 NVDEC off VIC_FREQ 0%@192 APE 25 "
           "PLL@36C CPU@38.5C PMIC@50C GPU@36C AO@44.5C thermal@37.5C POM_5V_IN 3373/3373 POM_5V_GPU 0/0\n";
}

struct Run {
    int exit_code = -1;  // -1: 아직, -2: 시그널
    std::vector<std::map<std::string, std::string>> beats;  // 받은 WATCHDOG
    bool ready = false;
    double ready_s = -1;  // 띄운 뒤 READY까지
};

static std::map<std::string, std::string> fields(const std::string& msg) {
    std::map<std::string, std::string> out;
    std::istringstream in(msg);
    for (std::string w; in >> w;) {
        const auto eq = w.find('=');
        if (eq != std::string::npos) out[w.substr(0, eq)] = w.substr(eq + 1);
    }
    return out;
}

static void write(const std::string& path, const std::string& text) { std::ofstream(path) << text; }

static void start(const Json& config, const std::map<std::string, std::string>& env) {
    write(g_dir + "/system-monitor.json", Json{{"id", "system-monitor"}, {"config_version", 1}, {"config", config}}.dump());
    g_pid = fork();
    if (g_pid == 0) {
        for (const auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        std::vector<std::string> args = g_cmd;
        args.push_back("--config");
        args.push_back(g_dir + "/system-monitor.json");
        std::vector<char*> av;
        for (auto& a : args) av.push_back(a.data());
        av.push_back(nullptr);
        execv(av[0], av.data());
        _exit(127);
    }
}

// until가 참이 되거나 워커가 끝날 때까지(최대 ms) heartbeat를 받는다
static Run& pump(Run& r, int ms, const std::function<bool(const Run&)>& until = nullptr) {
    const auto t0 = Clock::now(), end = t0 + std::chrono::milliseconds(ms);
    char buf[8192];
    while (Clock::now() < end && r.exit_code == -1) {
        const ssize_t n = recv(g_sock, buf, sizeof buf - 1, MSG_DONTWAIT);
        if (n > 0) {
            const std::string msg(buf, static_cast<size_t>(n));
            if (msg.rfind("READY=1", 0) == 0) r.ready = true;
            if (msg.rfind("WATCHDOG=1", 0) == 0) r.beats.push_back(fields(msg));
            continue;
        }
        int st = 0;
        if (g_pid > 0 && waitpid(g_pid, &st, WNOHANG) == g_pid) {
            r.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -2;
            g_pid = -1;
            break;
        }
        if (until && until(r)) break;
        std::this_thread::sleep_for(20ms);
    }
    return r;
}

static Run run_until_ready(const Json& config, const std::map<std::string, std::string>& env) {
    Run r;
    const auto t0 = Clock::now();
    start(config, env);
    pump(r, 12000, [](const Run& x) { return x.ready; });
    r.ready_s = std::chrono::duration<double>(Clock::now() - t0).count();
    return r;
}

static int stop(Run& r) {
    if (g_pid > 0) kill(g_pid, SIGTERM);
    pump(r, 6000);
    return r.exit_code;
}

static bool alive(const std::string& pid_file) {
    std::ifstream in(pid_file);
    int pid = 0;
    return in >> pid && pid > 0 && kill(pid, 0) == 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_worker [emulator...] <sysmon>\n");
        return 2;
    }
    g_cmd.assign(argv + 1, argv + argc);
    const bool emulated = g_cmd.size() > 1;
    // 시험이 자식의 PATH를 바꾸므로 첫 인자(에뮬레이터 등)는 지금 PATH로 절대 경로로 바꿔 둔다
    if (g_cmd[0].find('/') == std::string::npos) {
        std::istringstream dirs(std::getenv("PATH") ? std::getenv("PATH") : "");
        for (std::string d; std::getline(dirs, d, ':');)
            if (access((d + "/" + g_cmd[0]).c_str(), X_OK) == 0) {
                g_cmd[0] = d + "/" + g_cmd[0];
                break;
            }
    }
    char tmpl[] = "/tmp/sysmon_test_XXXXXX";
    g_dir = mkdtemp(tmpl);
    std::atexit(cleanup);
    for (const char* d : {"/run", "/run/cmd", "/run/stream", "/bin", "/empty"}) mkdir((g_dir + d).c_str(), 0755);
    setenv("WORKSPACE_DATA", g_dir.c_str(), 1);
    setenv("WORKSPACE_WORKER_ID", "system-monitor", 1);
    setenv("FAKE_DIR", g_dir.c_str(), 1);
    unsetenv("WORKSPACE_PEERS_FILE");
    unsetenv("WORKSPACE_INPUTS_FILE");

    // 가짜 Manager heartbeat 소켓
    const std::string notify = g_dir + "/notify.sock";
    g_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::snprintf(a.sun_path, sizeof a.sun_path, "%s", notify.c_str());
    if (bind(g_sock, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        std::perror("bind");
        return 1;
    }
    setenv("WORKSPACE_NOTIFY_SOCKET", notify.c_str(), 1);

    const std::string fake = g_dir + "/bin/tegrastats";
    write(fake, kFake);
    chmod(fake.c_str(), 0755);
    const std::string path_with_fake = g_dir + "/bin:" + (std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin");
    const Json cfg = {{"sysmon", {{"interval_ms", 200}}}, {"watchdog_sec", 1}};

    // 2) 설정 오류 → exit 1(띄우기 전에 끝남)
    for (const Json& bad : {Json{{"sysmon", {{"interval_ms", 50}}}}, Json{{"sysmon", {{"rate", 1}}}}, Json{{"interval", 1}},
                            Json{{"sysmon", {{"interval_ms", "x"}}}}}) {
        Run r;
        start(bad, {{"PATH", g_dir + "/empty"}});
        CHECK(pump(r, 5000).exit_code == 1);
        CHECK(!r.ready);
    }

    if (!emulated) {
        // 1) 직접 실행
        {
            Run r = run_until_ready(cfg, {{"PATH", path_with_fake}, {"FAKE_MODE", "normal"}});
            CHECK(r.ready);
            CHECK(r.ready_s >= 0.5);  // 가짜는 0.6 s 뒤 첫 줄: READY는 그 뒤
            pump(r, 3000, [](const Run& x) { return x.beats.size() >= 2; });
            CHECK(r.beats.size() >= 2);
            if (!r.beats.empty()) {
                auto& b = r.beats.back();
                std::printf("heartbeat: progress=%s ram_used_mb=%s cpu_avg_pct=%s temp_max_c=%s collector_root=%s\n",
                            b["progress"].c_str(), b["ram_used_mb"].c_str(), b["cpu_avg_pct"].c_str(), b["temp_max_c"].c_str(),
                            b["collector_root"].c_str());
                CHECK(b["ram_used_mb"] == "3456");
                CHECK(b["ram_total_mb"] == "7620");
                CHECK(b["swap_used_mb"] == "12");
                CHECK(b["swap_total_mb"] == "3810");
                CHECK(b["cpu0_pct"] == "11");
                CHECK(b["cpu0_mhz"] == "1497");
                CHECK(b.count("cpu1_pct") == 0);  // off
                CHECK(b["cpu3_mhz"] == "729");
                CHECK(b["cpu_online"] == "3");
                CHECK(b["cpu_avg_pct"] == "5.3");
                CHECK(b["cpu_max_pct"] == "11");
                CHECK(b["cpu_mhz_max"] == "1497");
                CHECK(b["gpu_pct"] == "0");
                CHECK(b["gpu_mhz"] == "305");
                CHECK(b["temp_cpu_c"] == "46.937");
                CHECK(b["temp_tj_c"] == "47.125");
                CHECK(b.count("temp_gpu_c") == 0);  // -256C
                CHECK(b["temp_max_c"] == "47.125");
                CHECK(b["power_vdd_in_mw"] == "4524");
                CHECK(b["power_vdd_cpu_gpu_cv_mw"] == "486");
                CHECK(b["collector_root"] == "0");
                CHECK(b.count("uptime_s") == 1);
                CHECK(b.count("disk_total_gb") == 1);
                CHECK(b.count("sysmon_ram_used_mb") == 0);  // 접두어 없이
                CHECK(b.count("emc_pct") == 0);
                CHECK(std::stoi(b["progress"]) >= 2);
            }
            std::ifstream args(g_dir + "/args");
            std::string got;
            std::getline(args, got);
            CHECK(got == "--interval 200");
            CHECK(alive(g_dir + "/pid"));
            CHECK(stop(r) == 0);
            std::this_thread::sleep_for(300ms);
            CHECK(!alive(g_dir + "/pid"));  // SIGTERM 정리: 자식 tegrastats도 끝남
        }
        // 옛 형식(맨 위 interval_ms)
        {
            Run r = run_until_ready({{"interval_ms", 300}}, {{"PATH", path_with_fake}, {"FAKE_MODE", "normal"}});
            CHECK(r.ready);
            std::ifstream args(g_dir + "/args");
            std::string got;
            std::getline(args, got);
            CHECK(got == "--interval 300");
            CHECK(stop(r) == 0);
        }
        // 3) 장치 오류 → exit 2
        {
            Run r;
            start(cfg, {{"PATH", g_dir + "/empty"}});  // tegrastats 없음
            CHECK(pump(r, 5000).exit_code == 2);
            CHECK(!r.ready);
        }
        {
            Run r;  // 알아볼 수 없는 줄만: 5 s + interval 뒤
            start(cfg, {{"PATH", path_with_fake}, {"FAKE_MODE", "garbage"}});
            CHECK(pump(r, 10000).exit_code == 2);
            CHECK(!r.ready);
        }
        {
            Run r = run_until_ready(cfg, {{"PATH", path_with_fake}, {"FAKE_MODE", "exit"}});  // 첫 줄 뒤 exit 3
            CHECK(r.ready);
            CHECK(pump(r, 5000).exit_code == 2);
        }
        {
            Run r = run_until_ready(cfg, {{"PATH", path_with_fake}, {"FAKE_MODE", "silent"}});  // 첫 줄 뒤 출력 없음
            CHECK(r.ready);
            const auto t0 = Clock::now();
            CHECK(pump(r, 6000).exit_code == 2);  // watchdog_sec 1 + interval 0.2 뒤
            CHECK(Clock::now() - t0 < 4s);
            std::this_thread::sleep_for(300ms);
            CHECK(!alive(g_dir + "/pid"));
        }
    } else {
        std::printf("emulated: fake tegrastats tests skipped\n");
    }

    // 4) root 수집기 파일
    const std::string line_file = g_dir + "/run/tegrastats.line";
    {
        write(line_file, jp4_line(7));
        Run r = run_until_ready(cfg, {{"PATH", g_dir + "/empty"}});  // tegrastats 없어도 수집기로 돈다
        CHECK(r.ready);
        // 수집기가 매 주기 파일을 바꾼다(100 ms마다, 1.5 s)
        for (int i = 0; i < 15; ++i) {
            write(line_file, jp4_line(10 + i));
            pump(r, 100);
        }
        CHECK(r.exit_code == -1);
        CHECK(!r.beats.empty());
        if (!r.beats.empty()) {
            auto& b = r.beats.back();
            CHECK(b["collector_root"] == "1");
            CHECK(std::stoi(b["emc_pct"]) >= 10);
            CHECK(b["nvenc_mhz"] == "1036");
            CHECK(b["nvdec_mhz"] == "0");
            CHECK(b["vic_mhz"] == "192");
            CHECK(b["gpu_pct"] == "9");
            CHECK(b["gpu_mhz"] == "921");
            CHECK(b["temp_cpu_c"] == "38.5");
            CHECK(b["temp_pmic_c"] == "50.0");
            CHECK(b["temp_max_c"] == "50.0");
            CHECK(b["cpu_avg_pct"] == "3.0");
            CHECK(b.count("power_pom_5v_in_mw") == 0);  // JP4 전력 표기(mW 없음)는 옛 판도 받지 않음
            CHECK(std::stoi(b["progress"]) >= 5);
        }
        // 수집기가 멈춤 → interval의 3배(0.6 s) 넘게 안 바뀌면 exit 2
        const auto t0 = Clock::now();
        CHECK(pump(r, 4000).exit_code == 2);
        CHECK(Clock::now() - t0 < 2500ms);
    }
    if (!emulated) {
        // 오래된 수집기 파일 → 직접 실행
        write(line_file, jp4_line(1));
        std::this_thread::sleep_for(800ms);  // interval 0.2 × 3 넘게
        Run r = run_until_ready(cfg, {{"PATH", path_with_fake}, {"FAKE_MODE", "normal"}});
        CHECK(r.ready);
        pump(r, 2000, [](const Run& x) { return !x.beats.empty(); });
        CHECK(!r.beats.empty() && r.beats.back()["collector_root"] == "0");
        CHECK(stop(r) == 0);
    }

    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("sysmon worker: all ok\n");
    return failures ? 1 : 0;
}
