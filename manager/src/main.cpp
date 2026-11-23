// process-manager 진입점. Python `python3 -m workspace_manager`와 같은 인자·환경 변수.
//
//     WORKSPACE_DATA=<데이터 폴더> process-manager [--data <폴더>] [--settings <파일>] [--log-level INFO]
//
// 스레드: 제어 루프(이 스레드, 상태 소유) + pm.pub(구독 송신) + pm.store(파일 쓰기) + pm.res(/proc 측정).
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "app.hpp"
#include "log.hpp"
#include "posix_fs.hpp"
#include "publisher.hpp"
#include "resources.hpp"
#include "settings.hpp"
#include "spawner.hpp"
#include "store.hpp"
#include "supervisor.hpp"
#include "worker/thread.hpp"

extern char** environ;

namespace {

class SteadyClock : public pm::Clock {
public:
    double mono() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::string now_iso() override {
        // 지역 시각 + 오프셋 "+09:00"(Python datetime.isoformat(timespec="seconds"))
        const time_t t = std::time(nullptr);
        tm lt{};
        localtime_r(&t, &lt);
        char buf[40];
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S%z", &lt);
        std::string s = buf;
        if (s.size() >= 5) s.insert(s.size() - 2, ":");
        return s;
    }
};

std::string env_or(const char* key, const std::string& dflt) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : dflt;
}

std::string read_boot_id() {
    std::string text, err;
    pm::PosixFs fs;
    if (fs.read("/proc/sys/kernel/random/boot_id", text, err) != pm::FileSystem::Read::Ok) return "";
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

void usage() {
    std::printf("usage: process-manager [-h] [--data DATA] [--settings SETTINGS] [--log-level LOG_LEVEL]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string data = env_or("WORKSPACE_DATA", "");
    std::string settings_path = env_or("WORKSPACE_MANAGER_SETTINGS", "");
    std::string level = env_or("WORKSPACE_LOG_LEVEL", "INFO");
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto take = [&](const char* name, std::string& out) {
            const std::string n = name;
            if (a == n) {
                if (i + 1 >= argc) {
                    std::fprintf(stderr, "process-manager: error: argument %s: expected one argument\n", name);
                    std::exit(2);
                }
                out = argv[++i];
                return true;
            }
            if (a.rfind(n + "=", 0) == 0) {
                out = a.substr(n.size() + 1);
                return true;
            }
            return false;
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        }
        if (take("--data", data) || take("--settings", settings_path) || take("--log-level", level)) continue;
        usage();
        std::fprintf(stderr, "process-manager: error: unrecognized arguments: %s\n", a.c_str());
        return 2;
    }
    pm::set_log_level(level);

    // signalfd로 받을 신호는 스레드를 만들기 전에 막는다(모든 스레드가 물려받음). 워커는 exec 전에 푼다
    sigset_t mask;
    sigemptyset(&mask);
    for (int s : {SIGCHLD, SIGTERM, SIGINT, SIGHUP}) sigaddset(&mask, s);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);

    if (settings_path.empty() && !data.empty()) settings_path = data + "/config/manager.json";
    pm::Json settings;
    try {
        settings = pm::load_settings(settings_path, data);
    } catch (const std::exception& e) {
        PM_ERROR("%s", e.what());
        return 1;
    }
    const std::string root = pm::abspath(env_or("WORKSPACE_ROOT", "."));
    const pm::Json wd = settings["worker_defaults"];
    pm::Json types = pm::load_types(settings["types_file"].get<std::string>(), settings["types_dir"].get<std::string>(), wd, root);
    pm::add_builtin_types(types, wd, root);
    pm::mark_requirements(types);
    const std::set<std::string> api_workers = pm::manager_api_workers(types);

    const bool product = env_or("WORKSPACE_PRODUCT", "0") == "1";
    const bool config_write = env_or("WORKSPACE_CONFIG_WRITE", product ? "0" : "1") == "1";

    pm::StatePaths paths{settings["state_dir"].get<std::string>(), settings["run_dir"].get<std::string>(),
                         settings["workers_file"].get<std::string>()};
    pm::mkdir_p(paths.run_dir + "/frames");
    for (const auto& d : {paths.state_dir, paths.workers_dir(), paths.logs_dir()}) pm::mkdir_p(d);
    const std::string notify_path = paths.run_dir + "/notify.sock";

    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) env.emplace_back(*e);

    SteadyClock clock;
    pm::PosixSpawner spawner;
    pm::PosixFs fs;
    pm::StoreThread store(2.0, 64);
    store.start();
    worker::ThreadWatch watch(std::chrono::milliseconds(3000));

    pm::SupervisorConfig sc;
    sc.settings = settings;
    sc.types = types;
    sc.paths = paths;
    sc.notify_path = notify_path;
    sc.base_env = env;
    sc.product = product;
    sc.workspace_root = root;
    sc.boot_id = read_boot_id();
    sc.manager_api_workers = api_workers;
    sc.peers_file = paths.run_dir + "/peers.json";
    pm::Supervisor sup(sc, clock, spawner, store, fs);
    sup.load();
    // 시작 때 쓴 파일(옮긴 설정, desired.json 등)이 다 써진 뒤 API를 연다(Python 판처럼 시작 상태가 파일에 먼저)
    {
        bool done = false;
        store.submit({}, [&done](bool, const std::string&) { done = true; });
        const double end = clock.mono() + 10.0;
        while (!done && clock.mono() < end) {
            pollfd p{store.wake_fd(), POLLIN, 0};
            ::poll(&p, 1, 100);
            store.poll(clock.mono());
        }
    }

    const pm::Json api = settings["api"];
    pm::Publisher pub(watch, api.value("subscriber_timeout_s", 5.0));
    pm::Resources res(watch);

    pm::AppConfig ac;
    ac.api = api;
    ac.tick_s = settings.value("tick_s", 0.2);
    ac.log_max_bytes = settings["log"].value("max_bytes", 10LL << 20);
    ac.log_backups = settings["log"].value("backups", 3);
    ac.run_dir = paths.run_dir;
    ac.logs_dir = paths.logs_dir();
    ac.notify_path = notify_path;
    ac.api_workers = api_workers;
    ac.product = product;
    ac.config_write = config_write;
    // 명령 길(docs/command-path.md): 연결 선언(설정 폴더의 connections.json) → run/peers.json
    {
        const pm::Json cf = settings.value("connections_file", pm::Json());
        const size_t slash = settings_path.rfind('/');
        const std::string conf_dir = slash == std::string::npos ? std::string(".") : settings_path.substr(0, slash);
        ac.connections_path = cf.is_string() ? cf.get<std::string>() : conf_dir + "/connections.json";
    }
    ac.peers_path = sc.peers_file;
    ac.command_guard = settings.value("command_guard", std::string("warn"));
    if (ac.command_guard != "off" && ac.command_guard != "warn" && ac.command_guard != "enforce") {
        PM_WARN("command_guard %s unknown (off|warn|enforce); using warn", ac.command_guard.c_str());
        ac.command_guard = "warn";
    }
    ac.boot_id = sc.boot_id.empty() ? pm::Json(nullptr) : pm::Json(sc.boot_id);
    int rc = 0;
    {
        pm::App app(ac, sup, store, pub, res, watch);
        std::string err;
        if (!app.open(err)) {
            PM_ERROR("cannot open sockets: %s", err.c_str());
            store.stop(2.0);
            return 1;
        }
        pub.start();
        res.start();
        rc = app.run();
        pub.stop();
        res.stop();
    }
    // 상태 파일을 다 쓰고 끝낸다(멈춘 디스크면 3 s 뒤 그냥 끝냄)
    store.stop(3.0);
    std::fflush(stderr);
    ::_exit(rc);  // 떼어 둔 pm.store 스레드가 있을 수 있어 소멸자를 돌리지 않는다
}
