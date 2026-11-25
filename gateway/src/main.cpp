// 게이트웨이 진입점. Python `python3 -m workspace_rest`와 같은 인자·환경 변수.
//
//     WORKSPACE_DATA=<데이터> gateway [--manager-socket P] [--bind A] [--port N] [--connect-timeout S]
//                                     [--command-timeout S] [--response-timeout S] [--log-level L]
//
// 환경 변수(지금 REST와 같음): WORKSPACE_DATA, WORKSPACE_ROOT, WORKSPACE_MANAGER_SOCKET, WORKSPACE_REST_BIND,
//   WORKSPACE_REST_PORT, WORKSPACE_REST_CONNECT_TIMEOUT, WORKSPACE_REST_COMMAND_TIMEOUT, WORKSPACE_REST_RESPONSE_TIMEOUT,
//   WORKSPACE_LOG_LEVEL, WORKSPACE_REST_TOKEN, WORKSPACE_UI, WORKSPACE_PRODUCT, WORKSPACE_UI_DIR, WORKSPACE_UI_MEDIA_URL,
//   WORKSPACE_UI_REFRESH_MS, WORKSPACE_UI_TITLE
// 게이트웨이에만: WORKSPACE_GATEWAY_SOCKET(Unix 소켓도 엶, nginx용), WORKSPACE_GATEWAY_TCP=0(TCP를 열지 않음, nginx 뒤), WORKSPACE_GATEWAY_THREADS(동시 요청 상한, 기본 48: SSE도 연결마다 하나를 차지해 화면 여러 개를 넉넉히),
//   WORKSPACE_AUTH=on|off(기본 off, 제품 모드는 늘 on), WORKSPACE_GATEWAY_CACHE=0(Manager 캐시 끔)
// 설정 명령: gateway user add|passwd|del|list, gateway token add|del (users_cli.hpp)
//
// 주 스레드: 신호(SIGTERM·SIGINT)를 기다리며 0.5 s마다 스레드 멈춤을 본다. 멈추면 기록하고 exit 4(시작 셸이 다시 띄움).
#include <malloc.h>
#include <signal.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "log.hpp"
#include "server.hpp"
#include "ui_rules.hpp"
#include "users_cli.hpp"

extern char** environ;

namespace {

const char* const kUsage =
    "usage: gateway [-h] [--manager-socket MANAGER_SOCKET] [--bind BIND] [--port PORT]\n"
    "               [--connect-timeout CONNECT_TIMEOUT] [--command-timeout COMMAND_TIMEOUT]\n"
    "               [--response-timeout RESPONSE_TIMEOUT] [--log-level LOG_LEVEL]\n";

[[noreturn]] void arg_error(const std::string& msg) {
    std::fprintf(stderr, "%sgateway: error: %s\n", kUsage, msg.c_str());
    std::exit(2);
}

double to_float(const std::string& name, const std::string& v) {
    double d;
    if (!gw::py_float(v, d)) arg_error("argument " + name + ": invalid float value: '" + v + "'");
    return d;
}

long long to_int(const std::string& name, const std::string& v) {
    long long n;
    if (!gw::py_int(v, n)) arg_error("argument " + name + ": invalid int value: '" + v + "'");
    return n;
}

// 실행 파일 위치에서 워크스페이스 뿌리(gateway/bin/gateway → 두 단계 위). 시작 셸은 WORKSPACE_ROOT를 넘긴다
std::string default_root() {
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string exe = buf;
    return gw::normpath(exe.substr(0, exe.rfind('/')) + "/../..");
}

}  // namespace

int main(int argc, char** argv) {
    using gw::env_or;
    // 설정 명령(gateway user …, gateway token …): 서버를 띄우지 않고 사용자 파일만 고친다
    if (const int rc = gw::users_cli(argc, argv); rc >= 0) return rc;
    const std::string data = env_or("WORKSPACE_DATA", "");
    std::map<std::string, std::string> opt = {
        {"--manager-socket", env_or("WORKSPACE_MANAGER_SOCKET", gw::path_join(gw::path_join(data, "run"), "manager-rest.sock"))},
        {"--bind", env_or("WORKSPACE_REST_BIND", "0.0.0.0")},
        {"--port", env_or("WORKSPACE_REST_PORT", "8080")},
        {"--connect-timeout", env_or("WORKSPACE_REST_CONNECT_TIMEOUT", "1.0")},
        {"--command-timeout", env_or("WORKSPACE_REST_COMMAND_TIMEOUT", "3.0")},
        {"--response-timeout", env_or("WORKSPACE_REST_RESPONSE_TIMEOUT", "2.0")},
        {"--log-level", env_or("WORKSPACE_LOG_LEVEL", "INFO")},
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::printf("%s", kUsage);
            return 0;
        }
        std::string value;
        bool has_value = false;
        const size_t eq = a.find('=');
        if (a.rfind("--", 0) == 0 && eq != std::string::npos) {
            value = a.substr(eq + 1);
            a = a.substr(0, eq);
            has_value = true;
        }
        // argparse처럼 겹치지 않는 앞부분도 받는다(--port → --po)
        std::vector<std::string> hits;
        for (const auto& kv : opt)
            if (kv.first == a) hits = {a};
        if (hits.empty() && a.rfind("--", 0) == 0)
            for (const auto& kv : opt)
                if (kv.first.rfind(a, 0) == 0) hits.push_back(kv.first);
        if (hits.size() != 1) arg_error(hits.empty() ? "unrecognized arguments: " + a : "ambiguous option: " + a);
        if (!has_value) {
            if (i + 1 >= argc) arg_error("argument " + hits[0] + ": expected one argument");
            value = argv[++i];
        }
        opt[hits[0]] = value;
    }

    gw::GatewayConfig cfg;
    cfg.data_dir = data;
    cfg.workspace = gw::abspath(env_or("WORKSPACE_ROOT", "").empty() ? default_root() : env_or("WORKSPACE_ROOT", ""));
    cfg.manager_socket = opt["--manager-socket"];
    cfg.bind = opt["--bind"];
    cfg.port = static_cast<int>(to_int("--port", opt["--port"]));
    cfg.connect_timeout = to_float("--connect-timeout", opt["--connect-timeout"]);
    cfg.command_timeout = to_float("--command-timeout", opt["--command-timeout"]);
    cfg.response_timeout = to_float("--response-timeout", opt["--response-timeout"]);
    gw::set_log_level(opt["--log-level"]);

    cfg.token = env_or("WORKSPACE_REST_TOKEN", "");
    cfg.ui_enabled = env_or("WORKSPACE_UI", "1") != "0";
    cfg.product = env_or("WORKSPACE_PRODUCT", "0") == "1";
    // 모드(docs/modes.md): 워크스페이스는 development, 제품은 늘 operation. operation은 제품 동작을 미리 볼 때(시험)
    cfg.access_mode = env_or("WORKSPACE_ACCESS_MODE", "development");
    if (cfg.access_mode != "development" && cfg.access_mode != "operation") {
        GW_ERROR("WORKSPACE_ACCESS_MODE=%s: must be development or operation", cfg.access_mode.c_str());
        return 2;
    }
    if (cfg.product && cfg.access_mode != "operation") cfg.access_mode = "operation";
    cfg.unix_socket = env_or("WORKSPACE_GATEWAY_SOCKET", "");
    cfg.tcp = env_or("WORKSPACE_GATEWAY_TCP", "1") != "0";  // 0: nginx가 앞에 있어 TCP를 열지 않음
    const long long threads = to_int("WORKSPACE_GATEWAY_THREADS", env_or("WORKSPACE_GATEWAY_THREADS", "48"));
    cfg.threads = static_cast<size_t>(threads < 1 ? 1 : threads);
    const std::string auth = env_or("WORKSPACE_AUTH", "off");
    if (auth != "off" && auth != "on") {
        GW_ERROR("WORKSPACE_AUTH=%s: must be on or off", auth.c_str());
        return 2;
    }
    cfg.auth_on = auth == "on";
    if (cfg.product && !cfg.auth_on) {  // 제품 묶음은 늘 켬(꺼 두고 내보내지 않게)
        GW_WARN("product mode: authentication forced on (WORKSPACE_AUTH=%s ignored)", auth.c_str());
        cfg.auth_on = true;
    }
    if (cfg.auth_on && !cfg.token.empty())
        GW_WARN("WORKSPACE_REST_TOKEN is ignored while authentication is on (use per-user tokens: gateway token add)");
    cfg.cache = env_or("WORKSPACE_GATEWAY_CACHE", "1") != "0";
    // 화면(ui.py): 값이 비어 있으면 기본값
    auto env_nonempty = [](const char* k, const std::string& d) {
        const std::string v = env_or(k, "");
        return v.empty() ? d : v;
    };
    cfg.ui.web = gw::abspath(env_nonempty("WORKSPACE_UI_DIR", gw::path_join(gw::path_join(cfg.workspace, "ui"), "web")));
    cfg.ui.workers = gw::path_join(cfg.workspace, "workers");
    cfg.ui.media_url = env_nonempty("WORKSPACE_UI_MEDIA_URL", "http://{host}:8889/{path}");
    cfg.ui.refresh_ms = to_int("WORKSPACE_UI_REFRESH_MS", env_nonempty("WORKSPACE_UI_REFRESH_MS", "1000"));
    cfg.ui.title = env_nonempty("WORKSPACE_UI_TITLE", "Workspace");
    // 시작할 때의 WORKSPACE_* (설정 탭 "다시 시작하면 적용"의 기준. 시작 셸이 workspace.env 키를 넘긴다)
    for (char** e = environ; e && *e; ++e) {
        const std::string kv = *e;
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.rfind("WORKSPACE_", 0) == 0) cfg.start_env[kv.substr(0, eq)] = kv.substr(eq + 1);
    }

    // malloc 아레나를 2개로: 스레드(gw.http ×20)마다 아레나가 생기면 쉬는 동안에도 메모리가 늘어난다(목표 5 MB 이하)
    mallopt(M_ARENA_MAX, 2);
    // 신호는 스레드를 만들기 전에 막는다(모든 스레드가 물려받고, 주 스레드만 sigtimedwait로 받음)
    ::signal(SIGPIPE, SIG_IGN);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);

    auto* gateway = new gw::Gateway(cfg);  // 끝낼 때 상한 안에 못 멈추면 소멸자 없이 나간다
    std::string err;
    if (!gateway->start(err)) {
        GW_ERROR("%s", err.c_str());
        return 1;
    }
    GW_INFO("listening on %s%s%s%s, manager %s%s%s%s",
            cfg.tcp ? (cfg.bind + ":" + std::to_string(gateway->port())).c_str() : "(no tcp)",
            cfg.unix_socket.empty() ? "" : " and ", cfg.unix_socket.c_str(), "", cfg.manager_socket.c_str(),
            cfg.auth_on ? ", auth on" : cfg.token.empty() ? ", auth off" : ", auth off (token required)",
            cfg.ui_enabled ? "" : ", UI off", cfg.product ? ", product mode" : "");

    while (true) {
        timespec ts{0, 500 * 1000 * 1000};
        siginfo_t info{};
        const int sig = sigtimedwait(&mask, &info, &ts);
        if (sig == SIGTERM || sig == SIGINT) break;
        gateway->tick();
        const std::string stalled = gateway->stalled();
        if (!stalled.empty()) {
            GW_ERROR("thread stalled: %s, exiting", stalled.c_str());
            std::_Exit(4);
        }
    }
    GW_INFO("stopping");
    if (!gateway->stop(3.0)) std::_Exit(0);  // 끝나지 않는 요청은 기다리지 않음(Python REST도 바로 끝남)
    delete gateway;
    return 0;
}
